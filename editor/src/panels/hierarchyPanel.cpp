#include "panels/hierarchyPanel.h"

#include "editorContext.h"
#include "history/editorHistory.h"
#include "scene/editorScene.h"
#include "widgets/widgets.h"

#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/core/components/nameComponent.h"

#include "imgui.h"

#include <cctype>
#include <string>

namespace
{
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
} // namespace

namespace
{
std::string lowercase(std::string text)
{
  for (char& c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
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

  // Scroll to a selection made elsewhere (the viewport), once.
  const uint32_t selectedId = context.hasSelection() ? context.selection.getId() : UINT32_MAX;
  const bool scrollToSelection = selectedId != m_shownSelection;
  m_shownSelection = selectedId;

  const std::string filter = lowercase(m_filter);
  sfs::Entity toDestroy;
  for (const sfs::Entity& entity : context.scene->entities())
  {
    const std::string name = displayName(entity);
    if (!filter.empty() && lowercase(name).find(filter) == std::string::npos)
      continue;

    // Ids are reused after destruction, so the label pairs the name with the
    // id to keep ImGui's widget ids stable and unique.
    const std::string label = name + "##" + std::to_string(entity.getId());
    const bool selected = selectedId == entity.getId();

    if (ImGui::Selectable(label.c_str(), selected))
    {
      context.selection = entity;
      m_shownSelection = entity.getId(); // picked here: already in view
    }
    else if (selected && scrollToSelection)
      ImGui::SetScrollHereY(0.5f);

    if (ImGui::BeginPopupContextItem())
    {
      context.selection = entity;
      if (ImGui::MenuItem("Delete"))
        toDestroy = entity;
      ImGui::EndPopup();
    }
  }

  // Right-click on empty space below the list.
  if (ImGui::BeginPopupContextWindow("HierarchyBackground",
                                     ImGuiPopupFlags_MouseButtonRight |
                                         ImGuiPopupFlags_NoOpenOverItems))
  {
    drawCreateMenu(context);
    ImGui::EndPopup();
  }

  if (toDestroy.isValid())
    context.history->change(
        [&]
        {
          if (context.hasSelection() && context.selection.getId() == toDestroy.getId())
            context.selection = {};
          context.scene->destroyEntity(toDestroy);
          context.scene->flushEntities();
        });
}

void HierarchyPanel::drawCreateMenu(EditorContext& context)
{
  using Primitive = EditorScene::Primitive;
  EditorScene& scene = *context.scene;

  // Each creation is one undo step, and selects the new entity.
  const auto create = [&](auto&& make)
  {
    context.history->change(
        [&]
        {
          context.selection = make();
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
}
