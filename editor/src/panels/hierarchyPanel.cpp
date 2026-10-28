#include "panels/hierarchyPanel.h"

#include "assets/assetDrop.h"
#include "editorContext.h"
#include "history/editorHistory.h"
#include "scene/editorScene.h"
#include "scene/prefabs.h"
#include "widgets/theme.h"
#include "widgets/widgets.h"

#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/core/components/nameComponent.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/runtime/rendering3d/renderSystem3D.h"
#include "engine/runtime/serialization/prefabComponents.h"

#include "imgui.h"
#include "imgui_internal.h" // TreeNodeSetOpen

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <utility>

namespace
{
// Drag and drop payload: the dragged entity's id.
using assetDrop::kEntityPayload;

std::string displayName(const sfs::Entity& entity)
{
  if (entity.hasComponent<sfs::NameComponent>())
  {
    const std::string& name = entity.getComponent<sfs::NameComponent>().name;
    if (!name.empty())
      return name;
  }
  return "Entity " + std::to_string(entity.getId());
}

std::string lowercase(std::string text)
{
  for (char& c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// A prefab dragged from the Assets panel, if that's the drag; else empty.
std::string draggedPrefab(const EditorContext& context)
{
  const sfs::AssetRecord* record =
      context.assets ? assetDrop::draggedAsset(ImGui::GetDragDropPayload(), *context.assets) : nullptr;
  return record && assetDrop::placeable(*record) ? prefabs::nameOf(record->id) : std::string();
}

// The entity being dragged, if the drag is one of ours.
sfs::Entity draggedEntity(EditorContext& context)
{
  const ImGuiPayload* payload = ImGui::GetDragDropPayload();
  if (!payload || !payload->IsDataType(kEntityPayload))
    return {};
  return context.scene->getEntity(*static_cast<const sfs::Entity::EntityId*>(payload->Data));
}
} // namespace

void HierarchyPanel::drawContents(EditorContext& context)
{
  if (!context.scene)
    return;

  if (ImGui::Button("+"))
    ImGui::OpenPopup("CreateEntity");
  if (ImGui::BeginPopup("CreateEntity"))
  {
    drawCreateMenu(context);
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  widgets::searchInput("filter", m_filter, "Filter");
  ImGui::Separator();

  // Each open scene (the scene, prefabs in their tabs) keeps its own scroll
  // position, open rows and the selection last shown: a child window per scene.
  if (context.scene != m_shownScene)
  {
    m_shownSelections[m_shownScene] = m_shownSelection;
    const auto shown = m_shownSelections.find(context.scene);
    m_shownSelection = shown != m_shownSelections.end() ? shown->second : UINT32_MAX;
    m_shownScene = context.scene;
  }
  ImGui::PushID(context.scene);
  ImGui::BeginChild("entities");

  // Reveal a selection made elsewhere (the viewport), once: open its parents
  // and scroll to it.
  const uint32_t selectedId = context.hasSelection() ? context.selection.getId() : UINT32_MAX;
  m_scrollToSelection = selectedId != m_shownSelection;
  m_shownSelection = selectedId;
  m_reveal.clear();
  if (m_scrollToSelection && context.hasSelection())
    for (sfs::Entity parent = sfs::hierarchy::parentOf(context.selection); parent.isValid();
         parent = sfs::hierarchy::parentOf(parent))
      m_reveal.insert(parent.getId());

  const std::vector<sfs::Entity> entities = context.scene->entities();
  if (m_filter.empty())
  {
    const sfs::hierarchy::EntityTree tree(entities);
    for (const sfs::Entity& root : tree.roots())
      drawTree(context, tree, root);
  }
  else
  {
    // Matches anywhere in the tree, as a flat list.
    const std::string filter = lowercase(m_filter);
    for (const sfs::Entity& entity : entities)
      if (lowercase(displayName(entity)).find(filter) != std::string::npos)
        drawRow(context, entity, false);
  }

  drawDropBelow(context);
  ImGui::EndChild();
  ImGui::PopID();
  applyPendingEdit(context);
}

void HierarchyPanel::drawTree(EditorContext& context,
                              const sfs::hierarchy::EntityTree& tree,
                              const sfs::Entity& entity)
{
  const std::vector<sfs::Entity>& children = tree.children(entity);
  if (!drawRow(context, entity, !children.empty()))
    return;
  for (const sfs::Entity& child : children)
    drawTree(context, tree, child);
  ImGui::TreePop();
}

bool HierarchyPanel::drawRow(EditorContext& context, const sfs::Entity& entity, bool hasChildren)
{
  // Ids are reused after destruction, so the label pairs the name with the id
  // to keep ImGui's widget ids stable and unique.
  const std::string name = displayName(entity);
  const std::string label = name + "##" + std::to_string(entity.getId());
  const bool selected = context.hasSelection() && context.selection == entity;

  // A click selects, the arrow opens, and a double-click frames the entity
  // and opens or closes it (handled below); AllowOverlap lets the focus
  // button on the row take clicks.
  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth |
                             ImGuiTreeNodeFlags_AllowOverlap;
  if (!hasChildren)
    flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
  if (selected)
    flags |= ImGuiTreeNodeFlags_Selected;
  if (m_reveal.contains(entity.getId()))
    ImGui::SetNextItemOpen(true);

  // Prefab instances in sage (their parts a little softer), missing ones red.
  std::optional<ImVec4> color;
  if (entity.hasComponent<sfs::MissingPrefabComponent>())
    color = ImVec4(1.0f, 0.55f, 0.45f, 1.0f);
  else if (entity.hasComponent<sfs::PrefabInstanceComponent>())
    color = ImGui::ColorConvertU32ToFloat4(theme::kAccent);
  else if (entity.hasComponent<sfs::PrefabLinkComponent>())
    color = ImVec4(0.67f, 0.84f, 0.63f, 0.75f);
  if (color)
    ImGui::PushStyleColor(ImGuiCol_Text, *color);
  const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
  if (color)
    ImGui::PopStyleColor();
  const bool rowHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
  {
    context.selection = entity;
    m_shownSelection = entity.getId(); // picked here: already in view
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
      // Frames it, and opens (or closes) it to show what's inside.
      context.focusRequest = entity;
      if (hasChildren)
        ImGui::TreeNodeSetOpen(ImGui::GetItemID(), !open);
    }
  }
  else if (selected && m_scrollToSelection)
    ImGui::SetScrollHereY(0.5f);

  if (ImGui::BeginDragDropSource())
  {
    const sfs::Entity::EntityId id = entity.getId();
    ImGui::SetDragDropPayload(kEntityPayload, &id, sizeof(id));
    ImGui::TextUnformatted(name.c_str());
    ImGui::EndDragDropSource();
  }
  if (ImGui::BeginDragDropTarget())
  {
    // Only highlight a drop that's allowed (not onto itself or its own child).
    const sfs::Entity dragged = draggedEntity(context);
    if (dragged.isValid() && sfs::hierarchy::canParent(dragged, entity) &&
        sfs::hierarchy::parentOf(dragged) != entity &&
        ImGui::AcceptDragDropPayload(kEntityPayload))
      m_pending = {PendingEdit::Kind::Reparent, dragged, entity};
    // A prefab from the Assets panel: placed as a child.
    if (const std::string prefab = draggedPrefab(context);
        !prefab.empty() && ImGui::AcceptDragDropPayload(assetDrop::kAssetPayload))
      m_pending = {PendingEdit::Kind::Place, {}, entity, prefab};
    ImGui::EndDragDropTarget();
  }

  if (ImGui::BeginPopupContextItem())
  {
    context.selection = entity;
    if (ImGui::MenuItem("Focus", "F"))
      context.focusRequest = entity;
    ImGui::Separator();
    if (ImGui::BeginMenu("Create Child"))
    {
      drawCreateMenu(context, entity);
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Unparent", nullptr, false, sfs::hierarchy::parentOf(entity).isValid()))
      m_pending = {PendingEdit::Kind::Reparent, entity, {}};
    if (ImGui::MenuItem("Create Prefab", nullptr, false, context.assets != nullptr))
      m_pending = {PendingEdit::Kind::MakePrefab, entity, {}};
    ImGui::SetItemTooltip("Save it (and everything under it) as a prefab in prefabs/; "
                          "or drag it into the Assets panel");
    ImGui::Separator();
    if (ImGui::MenuItem("Delete"))
      m_pending = {PendingEdit::Kind::Delete, entity, {}};
    ImGui::EndPopup();
  }

  if (rowHovered || selected)
    drawFocusButton(context, entity);

  return open && hasChildren;
}

void HierarchyPanel::drawFocusButton(EditorContext& context, const sfs::Entity& entity)
{
  // A small crosshair at the row's right end.
  const float size = ImGui::GetFrameHeight() * 0.8f;
  const ImVec2 rowMin = ImGui::GetItemRectMin(), rowMax = ImGui::GetItemRectMax();
  const ImVec2 at(rowMax.x - size - ImGui::GetStyle().FramePadding.x,
                  rowMin.y + (rowMax.y - rowMin.y - size) * 0.5f);
  const ImVec2 cursor = ImGui::GetCursorScreenPos();
  ImGui::SetCursorScreenPos(at);
  if (ImGui::InvisibleButton("focus", ImVec2(size, size)))
    context.focusRequest = entity;
  const bool hovered = ImGui::IsItemHovered();
  ImGui::SetItemTooltip("Focus: frame it in the viewport (F, or double-click)");
  ImGui::SetCursorScreenPos(cursor);

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 centre(at.x + size * 0.5f, at.y + size * 0.5f);
  const ImU32 color = hovered ? theme::kAccent : ImGui::GetColorU32(ImGuiCol_TextDisabled);
  if (hovered)
    draw->AddCircleFilled(centre, size * 0.5f, theme::kAccentSoft);
  widgets::drawTarget(draw, centre, size, color);
}

void HierarchyPanel::drawDropBelow(EditorContext& context)
{
  // The space under the list: drop here to make an entity a root, right-click
  // to create one.
  const ImVec2 available = ImGui::GetContentRegionAvail();
  ImGui::InvisibleButton("##below",
                         ImVec2(std::max(available.x, 1.0f),
                                std::max(available.y, ImGui::GetFrameHeight())));

  if (ImGui::BeginDragDropTarget())
  {
    const sfs::Entity dragged = draggedEntity(context);
    if (dragged.isValid() && sfs::hierarchy::parentOf(dragged).isValid() &&
        ImGui::AcceptDragDropPayload(kEntityPayload))
      m_pending = {PendingEdit::Kind::Reparent, dragged, {}};
    if (const std::string prefab = draggedPrefab(context);
        !prefab.empty() && ImGui::AcceptDragDropPayload(assetDrop::kAssetPayload))
      m_pending = {PendingEdit::Kind::Place, {}, {}, prefab};
    ImGui::EndDragDropTarget();
  }

  if (ImGui::BeginPopupContextItem("HierarchyBackground"))
  {
    drawCreateMenu(context);
    ImGui::EndPopup();
  }
}

void HierarchyPanel::applyPendingEdit(EditorContext& context)
{
  const PendingEdit edit = std::exchange(m_pending, {});
  if (edit.kind == PendingEdit::Kind::Place)
  {
    prefabs::place(context, edit.prefab, edit.parent.isValid() && edit.parent.isAlive() ? edit.parent : sfs::Entity());
    m_shownSelection = UINT32_MAX; // reveal it
    return;
  }
  if (!edit.entity.isValid() || !edit.entity.isAlive())
    return;

  switch (edit.kind)
  {
  case PendingEdit::Kind::None:
    break;
  case PendingEdit::Kind::Reparent:
    context.history->change(
        [&]
        {
          sfs::hierarchy::setParentKeepWorld(edit.entity, edit.parent);
          context.selection = edit.entity;
        });
    m_shownSelection = UINT32_MAX; // reveal it under its new parent
    break;
  case PendingEdit::Kind::Delete:
    context.history->change(
        [&]
        {
          if (context.hasSelection() && (context.selection == edit.entity ||
                                         sfs::hierarchy::isAncestor(edit.entity, context.selection)))
            context.selection = {};
          context.scene->destroyBranch(edit.entity);
        });
    break;
  case PendingEdit::Kind::MakePrefab:
    prefabs::create(context, edit.entity, "prefabs");
    break;
  case PendingEdit::Kind::Place:
    break; // handled above
  }
}

void HierarchyPanel::drawCreateMenu(EditorContext& context, const sfs::Entity& parent)
{
  using Primitive = EditorScene::Primitive;
  EditorScene& scene = *context.scene;

  // Each creation is one undo step, and selects the new entity. A child starts
  // at its parent's origin, except one placed in the world (a camera at the
  // current view), which stays put.
  const auto create = [&](auto&& make, bool placedInWorld = false)
  {
    context.history->change(
        [&]
        {
          sfs::Entity entity = make();
          if (parent.isValid() && placedInWorld)
            sfs::hierarchy::setParentKeepWorld(entity, parent);
          else if (parent.isValid())
            sfs::hierarchy::setParent(entity, parent);
          context.selection = entity;
          scene.flushEntities();
        });
  };

  if (ImGui::MenuItem("Empty"))
    create([&] { return scene.createEmpty("Empty"); });
  ImGui::Separator();
  if (ImGui::MenuItem("Cube"))
    create([&] { return scene.createPrimitive(Primitive::Cube, "Cube"); });
  if (ImGui::MenuItem("Sphere"))
    create([&] { return scene.createPrimitive(Primitive::Sphere, "Sphere"); });
  if (ImGui::MenuItem("Cylinder"))
    create([&] { return scene.createPrimitive(Primitive::Cylinder, "Cylinder"); });
  if (ImGui::MenuItem("Plane"))
    create([&] { return scene.createPrimitive(Primitive::Plane, "Plane"); });
  ImGui::Separator();
  if (ImGui::MenuItem("Point Light"))
    create([&] { return scene.createPointLight("Point Light"); });
  if (ImGui::MenuItem("Camera"))
    create([&] { return scene.createCamera("Camera", context.render->camera()); }, true);
  ImGui::SetItemTooltip("A game camera, looking the way the viewport does");
}
