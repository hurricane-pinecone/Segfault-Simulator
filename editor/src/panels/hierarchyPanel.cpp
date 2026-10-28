#include "panels/hierarchyPanel.h"

#include "editorContext.h"
#include "history/editorHistory.h"
#include "scene/editorScene.h"
#include "widgets/widgets.h"

#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/core/components/nameComponent.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/runtime/rendering3d/renderSystem3D.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace
{
// Drag and drop payload: the dragged entity's id.
constexpr const char* kEntityPayload = "SFS_ENTITY";

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
  ImGui::SetNextItemWidth(-1.0f);
  widgets::inputText("##filter", m_filter);
  if (m_filter.empty() && !ImGui::IsItemActive())
  {
    // Placeholder text over the empty box.
    const ImVec2 min = ImGui::GetItemRectMin();
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(min.x + ImGui::GetStyle().FramePadding.x, min.y + ImGui::GetStyle().FramePadding.y),
        ImGui::GetColorU32(ImGuiCol_TextDisabled), "Filter");
  }
  ImGui::Separator();

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

  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                             ImGuiTreeNodeFlags_SpanFullWidth;
  if (!hasChildren)
    flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
  if (selected)
    flags |= ImGuiTreeNodeFlags_Selected;
  if (m_reveal.contains(entity.getId()))
    ImGui::SetNextItemOpen(true);

  const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
  {
    context.selection = entity;
    m_shownSelection = entity.getId(); // picked here: already in view
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
    ImGui::EndDragDropTarget();
  }

  if (ImGui::BeginPopupContextItem())
  {
    context.selection = entity;
    if (ImGui::BeginMenu("Create Child"))
    {
      drawCreateMenu(context, entity);
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Unparent", nullptr, false, sfs::hierarchy::parentOf(entity).isValid()))
      m_pending = {PendingEdit::Kind::Reparent, entity, {}};
    ImGui::Separator();
    if (ImGui::MenuItem("Delete"))
      m_pending = {PendingEdit::Kind::Delete, entity, {}};
    ImGui::EndPopup();
  }

  return open && hasChildren;
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
