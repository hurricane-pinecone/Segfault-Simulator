#include "panels/inspectorPanel.h"

#include "editorContext.h"
#include "widgets/widgets.h"

#include "engine/core/components/nameComponent.h"
#include "engine/core/components/scriptComponent.h"
#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/core/reflection/reflection.h"
#include "engine/runtime/rendering3d/meshLibrary.h"
#include "engine/runtime/rendering3d/renderSystem3D.h"
#include "engine/runtime/game/gameObject.h"
#include "engine/runtime/scripting/scriptRegistry.h"
#include "engine/runtime/serialization/sceneSerializer.h"
#include "glm/glm/gtc/quaternion.hpp"
#include "glm/glm/trigonometric.hpp"

#include "imgui.h"

#include <memory>
#include <string>

namespace
{

using sfs::reflect::FieldInfo;
using sfs::reflect::FieldKind;
using sfs::reflect::TypeInfo;

constexpr const char* kNameType = "NameComponent";

void meshField(const FieldInfo& field,
               void* object,
               sfs::MeshLibrary* meshes,
               EditorHistory* history)
{
  sfs::MeshHandle& mesh = field.in<sfs::MeshHandle>(object);
  std::string current =
      meshes && mesh.valid() ? std::string(meshes->nameOf(mesh)) : std::string();
  // A stand-in: usually a mesh the game module provides, before it's built.
  if (meshes && meshes->isMissing(mesh))
    current += " (missing)";
  const char* preview = mesh.valid() ? (current.empty() ? "(unnamed)" : current.c_str())
                                     : "None";

  if (!ImGui::BeginCombo(field.displayName.c_str(), preview))
    return;
  const auto choose = [&](sfs::MeshHandle chosen)
  {
    if (history)
      history->change([&] { mesh = chosen; });
    else
      mesh = chosen;
  };
  if (ImGui::Selectable("None", !mesh.valid()))
    choose({});
  if (meshes)
    for (const auto& [handle, name] : meshes->named())
      if (ImGui::Selectable(name.c_str(), handle == mesh))
        choose(handle);
  ImGui::EndCombo();
}

// A [[sfs::script]] string: pick one of the game object scripts.
void scriptField(const FieldInfo& field, void* object, EditorHistory* history)
{
  std::string& script = field.in<std::string>(object);
  const char* preview = script.empty() ? "None" : script.c_str();

  if (!ImGui::BeginCombo(field.displayName.c_str(), preview))
    return;
  const auto choose = [&](const std::string& chosen)
  {
    if (history)
      history->change([&] { script = chosen; });
    else
      script = chosen;
  };
  if (ImGui::Selectable("None", script.empty()))
    choose({});
  for (const sfs::ScriptType& type : sfs::scripts().types())
    if (type.kind == sfs::ScriptType::Kind::GameObject &&
        ImGui::Selectable(type.name.c_str(), type.name == script))
      choose(type.name);
  ImGui::EndCombo();
}

// One field, with the widget its kind and attributes call for. Every edit is
// recorded for undo.
void fieldWidget(const FieldInfo& field,
                 void* object,
                 sfs::MeshLibrary* meshes,
                 EditorHistory* history)
{
  const char* label = field.displayName.c_str();
  const auto& attributes = field.attributes;
  using widgets::undoable;

  switch (field.kind)
  {
  case FieldKind::Bool:
    undoable(history, field.in<bool>(object),
             [&](bool& v) { return ImGui::Checkbox(label, &v); });
    break;
  case FieldKind::Int:
    undoable(history, field.in<int32_t>(object), [&](int32_t& v) {
      return attributes.hasRange
                 ? ImGui::SliderInt(label, &v, static_cast<int>(attributes.min),
                                    static_cast<int>(attributes.max))
                 : ImGui::DragInt(label, &v);
    });
    break;
  case FieldKind::UInt:
    undoable(history, field.in<uint32_t>(object), [&](uint32_t& v)
             { return ImGui::DragScalar(label, ImGuiDataType_U32, &v); });
    break;
  case FieldKind::Float:
    undoable(history, field.in<float>(object), [&](float& v) {
      return attributes.hasRange
                 ? ImGui::SliderFloat(label, &v, attributes.min, attributes.max)
                 : ImGui::DragFloat(label, &v, 0.05f);
    });
    break;
  case FieldKind::String:
    if (attributes.script)
      scriptField(field, object, history);
    else
      undoable(history, field.in<std::string>(object),
               [&](std::string& v) { return widgets::inputText(label, v); });
    break;
  case FieldKind::Vec2:
    undoable(history, field.in<glm::vec2>(object),
             [&](glm::vec2& v) { return ImGui::DragFloat2(label, &v.x, 0.05f); });
    break;
  case FieldKind::Vec3:
    undoable(history, field.in<glm::vec3>(object), [&](glm::vec3& v) {
      return attributes.color ? ImGui::ColorEdit3(label, &v.x)
                              : ImGui::DragFloat3(label, &v.x, 0.05f);
    });
    break;
  case FieldKind::Vec4:
    undoable(history, field.in<glm::vec4>(object), [&](glm::vec4& v) {
      return attributes.color ? ImGui::ColorEdit4(label, &v.x)
                              : ImGui::DragFloat4(label, &v.x, 0.05f);
    });
    break;
  case FieldKind::Quat:
    // Edited as Euler angles in degrees; stored as a quaternion.
    undoable(history, field.in<glm::quat>(object), [&](glm::quat& rotation) {
      glm::vec3 degrees = glm::degrees(glm::eulerAngles(rotation));
      if (!ImGui::DragFloat3(label, &degrees.x, 0.5f))
        return false;
      rotation = glm::quat(glm::radians(degrees));
      return true;
    });
    break;
  case FieldKind::Mesh:
    meshField(field, object, meshes, history);
    break;
  case FieldKind::Json:
    break; // free-form data, edited by whatever owns it (e.g. scriptFields)
  }
}

// The attached script's own fields: a game object script's reflected fields,
// with this entity's values (kept in ScriptComponent::fields). Edited through a
// scratch instance of the script, so its widgets are the components' ones.
void scriptFields(sfs::ScriptComponent& component, sfs::MeshLibrary* meshes, EditorHistory* history)
{
  if (component.script.empty())
    return;
  const TypeInfo* type = sfs::reflect::types().find(component.script);
  const sfs::ScriptType* script = sfs::scripts().find(component.script);
  if (!type || !script || !script->createObject)
  {
    ImGui::TextDisabled("Build the scripts (Ctrl+B) to edit this script's fields");
    return;
  }
  if (type->fields.empty())
    return;

  // The class's defaults, overridden by what this entity has saved.
  const std::unique_ptr<sfs::GameObject> object = script->createObject();
  const sfs::SceneSerializer serializer(sfs::reflect::types(), meshes);
  serializer.loadObject(*type, object.get(), component.fields);
  nlohmann::json current = serializer.saveObject(*type, object.get());

  ImGui::SeparatorText(type->displayName.c_str());
  for (const FieldInfo& field : type->fields)
  {
    ImGui::PushID(field.name.c_str());
    fieldWidget(field, object.get(), meshes, nullptr);

    // Bracketed like widgets::undoable: a drag is one undo step.
    const nlohmann::json edited = serializer.saveObject(*type, object.get());
    const bool changed = edited != current;
    if (history && (ImGui::IsItemActivated() || changed))
      history->beginChange();
    if (changed)
    {
      component.fields = edited;
      current = edited;
    }
    if (history && (ImGui::IsItemDeactivated() || (changed && !ImGui::IsItemActive())))
      history->endChange();
    ImGui::PopID();
  }
}

// A collapsible section for one component, with Remove in its context menu.
void componentSection(const TypeInfo& type,
                      const sfs::Entity& entity,
                      sfs::MeshLibrary* meshes,
                      EditorHistory* history)
{
  ImGui::PushID(type.name.c_str());
  const bool expanded =
      ImGui::CollapsingHeader(type.displayName.c_str(), ImGuiTreeNodeFlags_DefaultOpen);

  bool remove = false;
  if (ImGui::BeginPopupContextItem())
  {
    remove = ImGui::MenuItem("Remove Component");
    ImGui::EndPopup();
  }

  if (remove)
    history->change([&] { type.component.remove(entity); });
  else if (expanded)
  {
    void* object = type.component.get(entity);
    for (const FieldInfo& field : type.fields)
    {
      // Each field in its own ID scope: a label may repeat the header's (the
      // Script component's "Script" field) and must not share its ID.
      ImGui::PushID(field.name.c_str());
      fieldWidget(field, object, meshes, history);
      ImGui::PopID();
    }
    if (type.fields.empty())
      ImGui::TextDisabled("No editable fields");
    if (type.name == "ScriptComponent")
      scriptFields(*static_cast<sfs::ScriptComponent*>(object), meshes, history);
  }
  ImGui::PopID();
}

void addComponentButton(const sfs::Entity& entity, EditorHistory* history)
{
  if (ImGui::Button("Add Component", ImVec2(-1.0f, 0.0f)))
    ImGui::OpenPopup("AddComponent");

  if (!ImGui::BeginPopup("AddComponent"))
    return;

  bool any = false;
  for (const TypeInfo& type : sfs::reflect::types().types())
  {
    if (type.kind != sfs::reflect::TypeKind::Component || type.name == kNameType ||
        type.component.has(entity))
      continue;
    any = true;
    if (ImGui::MenuItem(type.displayName.c_str()))
      history->change([&] { type.component.add(entity); });
  }
  if (!any)
    ImGui::TextDisabled("Every component is already added");
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

  const sfs::Entity& entity = context.selection;
  sfs::MeshLibrary* meshes = context.render ? &context.render->meshes() : nullptr;

  EditorHistory* history = context.history;

  if (!entity.hasComponent<sfs::NameComponent>())
    sfs::Entity(entity).addComponent<sfs::NameComponent>();
  widgets::undoable(history, entity.getComponent<sfs::NameComponent>().name,
                    [](std::string& name) { return widgets::inputText("Name", name); });
  ImGui::Separator();

  for (const TypeInfo& type : sfs::reflect::types().types())
    if (type.kind == sfs::reflect::TypeKind::Component && type.name != kNameType &&
        type.component.has(entity))
      componentSection(type, entity, meshes, history);

  ImGui::Spacing();
  addComponentButton(entity, history);
}
