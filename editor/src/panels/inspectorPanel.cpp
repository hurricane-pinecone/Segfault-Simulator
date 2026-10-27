#include "panels/inspectorPanel.h"

#include "editorContext.h"

#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/components/nameComponent.h"
#include "engine/core/components/pointLight3DComponent.h"
#include "engine/core/components/transform3DComponent.h"
#include "glm/glm/gtc/quaternion.hpp"
#include "glm/glm/trigonometric.hpp"

#include "imgui.h"
#include "widgets/widgets.h"

namespace
{

// A collapsible section for component T, with a context menu to remove it.
// `edit` draws the fields; it isn't called once the component is removed.
template <typename T, typename Edit>
void componentSection(sfs::Entity& entity, const char* label, Edit&& edit)
{
  if (!entity.hasComponent<T>())
    return;

  const bool expanded =
      ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);

  bool remove = false;
  if (ImGui::BeginPopupContextItem())
  {
    if (ImGui::MenuItem("Remove Component"))
      remove = true;
    ImGui::EndPopup();
  }

  if (remove)
  {
    entity.removeComponent<T>();
    return;
  }

  if (expanded)
  {
    ImGui::PushID(label);
    edit(entity.getComponent<T>());
    ImGui::PopID();
  }
}

void editTransform(sfs::Transform3DComponent& transform)
{
  ImGui::DragFloat3("Position", &transform.position.x, 0.05f);

  // Edited as Euler angles in degrees; stored as a quaternion.
  glm::vec3 degrees = glm::degrees(glm::eulerAngles(transform.rotation));
  if (ImGui::DragFloat3("Rotation", &degrees.x, 0.5f))
    transform.rotation = glm::quat(glm::radians(degrees));

  ImGui::DragFloat3("Scale", &transform.scale.x, 0.02f, 0.001f, 1000.0f);
}

void editMeshRenderer(sfs::MeshRendererComponent& renderer)
{
  ImGui::ColorEdit4("Tint", &renderer.tint.x);
  ImGui::Checkbox("Casts Shadow", &renderer.castsShadow);
  ImGui::TextDisabled("Mesh #%u", renderer.mesh.id);
}

void editPointLight(sfs::PointLight3DComponent& light)
{
  ImGui::ColorEdit3("Color", &light.color.x);
  ImGui::DragFloat("Intensity", &light.intensity, 0.01f, 0.0f, 10.0f);
  ImGui::DragFloat("Radius", &light.radius, 0.05f, 0.0f, 200.0f);
  ImGui::DragFloat3("Offset", &light.offset.x, 0.05f);
}

void addComponentMenu(sfs::Entity& entity)
{
  if (ImGui::Button("Add Component", ImVec2(-1.0f, 0.0f)))
    ImGui::OpenPopup("AddComponent");

  if (!ImGui::BeginPopup("AddComponent"))
    return;

  if (!entity.hasComponent<sfs::Transform3DComponent>() &&
      ImGui::MenuItem("Transform"))
    entity.addComponent<sfs::Transform3DComponent>();
  if (!entity.hasComponent<sfs::PointLight3DComponent>() &&
      ImGui::MenuItem("Point Light"))
    entity.addComponent<sfs::PointLight3DComponent>();
  ImGui::EndPopup();
}

} // namespace

void InspectorPanel::drawContents(EditorContext& context)
{
  if (!context.hasSelection())
  {
    ImGui::TextDisabled("Nothing selected");
    return;
  }

  sfs::Entity& entity = context.selection;

  if (!entity.hasComponent<sfs::NameComponent>())
    entity.addComponent<sfs::NameComponent>();
  widgets::inputText("Name", entity.getComponent<sfs::NameComponent>().name);
  ImGui::Separator();

  componentSection<sfs::Transform3DComponent>(entity, "Transform", editTransform);
  componentSection<sfs::MeshRendererComponent>(
      entity, "Mesh Renderer", editMeshRenderer);
  componentSection<sfs::PointLight3DComponent>(
      entity, "Point Light", editPointLight);

  ImGui::Spacing();
  addComponentMenu(entity);
}
