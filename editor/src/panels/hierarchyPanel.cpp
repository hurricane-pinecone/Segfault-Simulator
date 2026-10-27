#include "panels/hierarchyPanel.h"

#include "editorContext.h"
#include "scene/editorScene.h"

#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/core/components/nameComponent.h"

#include "imgui.h"

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
  ImGui::Separator();

  sfs::Entity toDestroy;
  for (const sfs::Entity& entity : context.scene->entities())
  {
    // Ids are reused after destruction, so the label pairs the name with the
    // id to keep ImGui's widget ids stable and unique.
    const std::string label =
        displayName(entity) + "##" + std::to_string(entity.getId());
    const bool selected =
        context.hasSelection() && context.selection.getId() == entity.getId();

    if (ImGui::Selectable(label.c_str(), selected))
      context.selection = entity;

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
  {
    if (context.hasSelection() && context.selection.getId() == toDestroy.getId())
      context.selection = {};
    context.scene->destroyEntity(toDestroy);
  }
}

void HierarchyPanel::drawCreateMenu(EditorContext& context)
{
  using Primitive = EditorScene::Primitive;
  EditorScene& scene = *context.scene;

  if (ImGui::MenuItem("Empty"))
    context.selection = scene.createEmpty("Empty");
  ImGui::Separator();
  if (ImGui::MenuItem("Cube"))
    context.selection = scene.createPrimitive(Primitive::Cube, "Cube");
  if (ImGui::MenuItem("Sphere"))
    context.selection = scene.createPrimitive(Primitive::Sphere, "Sphere");
  if (ImGui::MenuItem("Cylinder"))
    context.selection = scene.createPrimitive(Primitive::Cylinder, "Cylinder");
  if (ImGui::MenuItem("Plane"))
    context.selection = scene.createPrimitive(Primitive::Plane, "Plane");
  ImGui::Separator();
  if (ImGui::MenuItem("Point Light"))
    context.selection = scene.createPointLight("Point Light");
}
