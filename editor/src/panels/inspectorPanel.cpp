#include "panels/inspectorPanel.h"

#include "assets/assetDrop.h"
#include "assets/materialFiles.h"
#include "assets/scriptableObjectFiles.h"
#include "editorContext.h"
#include "platform/shell.h"
#include "scene/editorScene.h"
#include "scene/prefabs.h"
#include "scripting/scriptFiles.h"
#include "widgets/theme.h"
#include "widgets/widgets.h"

#include "engine/core/components/boxCollider3DComponent.h"
#include "engine/core/components/capsuleCollider3DComponent.h"
#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/components/nameComponent.h"
#include "engine/core/components/scriptComponent.h"
#include "engine/core/components/sphereCollider3DComponent.h"
#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/logger/logger.h"
#include "engine/core/reflection/reflection.h"
#include "engine/core/rendering/aabb.h"
#include "engine/core/rendering/emissionLights.h"
#include "engine/runtime/assetStore/modelData.h"
#include "engine/runtime/assetStore/textureImporter.h"
#include "engine/runtime/game/gameObject.h"
#include "engine/runtime/rendering/meshLibrary.h"
#include "engine/runtime/rendering/renderSystem.h"
#include "engine/runtime/scriptableObjects/scriptableObjects.h"
#include "engine/runtime/scripting/scriptRegistry.h"
#include "engine/runtime/serialization/prefabComponents.h"
#include "engine/runtime/serialization/sceneSerializer.h"
#include "glm/glm/gtc/quaternion.hpp"
#include "glm/glm/matrix.hpp"
#include "glm/glm/trigonometric.hpp"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

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
  std::string current = meshes && mesh.valid()
                            ? meshes->label(meshes->nameOf(mesh))
                            : std::string();
  // A stand-in: usually a mesh the game module provides, before it's built.
  if (meshes && meshes->isMissing(mesh))
    current += " (missing)";
  const char* preview =
      mesh.valid() ? (current.empty() ? "(unnamed)" : current.c_str()) : "None";

  const auto choose = [&](sfs::MeshHandle chosen)
  {
    if (history)
      history->change([&] { mesh = chosen; });
    else
      mesh = chosen;
  };
  widgets::propertyLabel(field.displayName.c_str());
  if (ImGui::BeginCombo("##value", preview))
  {
    if (ImGui::Selectable("None", !mesh.valid()))
      choose({});
    if (meshes)
    {
      for (const auto& [handle, name] : meshes->named())
        if (ImGui::Selectable(meshes->label(name).c_str(), handle == mesh))
          choose(handle);
      // Meshes that load when picked (model assets).
      const auto available = meshes->available();
      if (!available.empty())
        ImGui::SeparatorText("Assets");
      for (const auto& [name, label] : available)
      {
        ImGui::PushID(name.c_str());
        if (ImGui::Selectable(label.c_str()))
          if (const sfs::MeshHandle loaded = meshes->resolve(name);
              loaded.valid())
            choose(loaded);
        ImGui::PopID();
      }
    }
    ImGui::EndCombo();
  }

  // Or a model dropped from the Assets panel: the whole model.
  const sfs::AssetDatabase* assets =
      sfs::scriptableObjects().catalogue(); // the project's
  if (meshes && assets && ImGui::BeginDragDropTarget())
  {
    const sfs::AssetRecord* dragged =
        assetDrop::draggedAsset(ImGui::GetDragDropPayload(), *assets);
    if (dragged && dragged->kind == sfs::AssetKind::Model &&
        ImGui::AcceptDragDropPayload(assetDrop::kAssetPayload))
      if (const sfs::MeshHandle loaded =
              meshes->resolve("asset:" + dragged->id.toString());
          loaded.valid())
        choose(loaded);
    ImGui::EndDragDropTarget();
  }
}

// A [[sfs::texture]] string: pick one of the project's textures (or none:
// the mesh's own).
void textureField(const FieldInfo& field,
                  void* object,
                  sfs::MeshLibrary* meshes,
                  EditorHistory* history)
{
  std::string& texture = field.in<std::string>(object);
  const std::string current = texture.empty() ? "None"
                              : meshes        ? meshes->label(texture)
                                              : texture;

  widgets::propertyLabel(field.displayName.c_str());
  if (!ImGui::BeginCombo("##value", current.c_str()))
    return;
  const auto choose = [&](const std::string& chosen)
  {
    if (history)
      history->change([&] { texture = chosen; });
    else
      texture = chosen;
  };
  if (ImGui::Selectable("None", texture.empty()))
    choose({});
  if (meshes)
    for (const auto& [name, label] : meshes->availableTextures())
    {
      ImGui::PushID(name.c_str());
      if (ImGui::Selectable(label.c_str(), name == texture))
        choose(name);
      ImGui::PopID();
    }
  ImGui::EndCombo();
}

// A [[sfs::script]] string: pick one of the game object scripts.
// What can be done with a script from its field: open it in the code panel,
// or show it in the Assets panel.
struct ScriptActions
{
  std::function<void(const std::string& script)> edit;
  std::function<void(const std::string& script)> reveal;
};

void scriptField(const FieldInfo& field,
                 void* object,
                 EditorHistory* history,
                 const ScriptActions* actions)
{
  std::string& script = field.in<std::string>(object);
  const char* preview = script.empty() ? "None" : script.c_str();

  // The label, the picker, then (with a script picked) icons at the right
  // edge to edit it and find it: the picker leaves room for them.
  const ImGuiStyle& style = ImGui::GetStyle();
  const bool icons = actions && !script.empty();
  const float iconsWidth =
      icons ? 2.0f * (ImGui::GetFrameHeight() + style.ItemInnerSpacing.x)
            : 0.0f;
  widgets::propertyLabel(field.displayName.c_str());
  ImGui::SetNextItemWidth(std::max(
      ImGui::GetContentRegionAvail().x - iconsWidth, ImGui::GetFrameHeight()));
  const bool picking = ImGui::BeginCombo("##script", preview);
  const std::string picked =
      script; // what the icons act on, before any pick below
  const auto iconsAfter = [&]
  {
    if (icons)
    {
      ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
      if (widgets::editButton("edit"))
        actions->edit(picked);
      ImGui::SetItemTooltip("Edit %s", picked.c_str());
      ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
      if (widgets::targetButton("reveal"))
        actions->reveal(picked);
      ImGui::SetItemTooltip("Show %s in the Assets panel", picked.c_str());
    }
  };
  if (!picking)
  {
    iconsAfter();
    return;
  }
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
  iconsAfter();
}

// A pick of the project's scriptable objects of `type` (or none). True when
// one's picked.
bool assetRefCombo(const char* label,
                   const std::string& type,
                   std::string& asset)
{
  const std::vector<sfs::ScriptableObjects::Entry> entries =
      sfs::scriptableObjects().list(type);
  std::string preview = "None";
  if (!asset.empty())
  {
    preview = "Missing";
    for (const sfs::ScriptableObjects::Entry& entry : entries)
      if (entry.asset == asset)
        preview = entry.name;
  }
  bool picked = false;
  if (ImGui::BeginCombo(label, preview.c_str()))
  {
    if (ImGui::Selectable("None", asset.empty()))
    {
      asset.clear();
      picked = true;
    }
    for (const sfs::ScriptableObjects::Entry& entry : entries)
    {
      ImGui::PushID(entry.asset.c_str());
      if (ImGui::Selectable(entry.name.c_str(), entry.asset == asset))
      {
        asset = entry.asset;
        picked = true;
      }
      ImGui::PopID();
    }
    if (entries.empty())
      ImGui::TextDisabled(
          "No %s assets yet (Assets: New Scriptable Object)", type.c_str());
    ImGui::EndCombo();
  }

  // Or dropped from the Assets panel: one of this type.
  const sfs::AssetDatabase* catalogue = sfs::scriptableObjects().catalogue();
  if (catalogue && ImGui::BeginDragDropTarget())
  {
    if (const sfs::AssetRecord* dragged =
            assetDrop::draggedAsset(ImGui::GetDragDropPayload(), *catalogue))
    {
      const std::string ref = "asset:" + dragged->id.toString();
      const bool fits =
          std::any_of(entries.begin(),
                      entries.end(),
                      [&](const auto& e) { return e.asset == ref; });
      if (fits && ImGui::AcceptDragDropPayload(assetDrop::kAssetPayload))
      {
        asset = ref;
        picked = true;
      }
    }
    ImGui::EndDragDropTarget();
  }
  return picked;
}

// What a material slot's icons do with its material asset: edit it (a graph
// in the node editor; a standard one in the Inspector) and find it in the
// Assets panel.
struct MaterialActions
{
  std::function<void(const sfs::AssetRecord& material)> edit;
  std::function<void(const sfs::AssetRecord& material)> reveal;
};

// The material asset a slot's material name ("asset:<id>") is, if it is one.
const sfs::AssetRecord* materialAsset(const std::string& name,
                                      const sfs::AssetDatabase* assets)
{
  const std::optional<sfs::AssetMeshName> asset =
      assets ? sfs::parseAssetMeshName(name) : std::nullopt;
  const sfs::AssetRecord* record =
      asset && asset->part.empty() ? assets->find(asset->model) : nullptr;
  return record && record->kind == sfs::AssetKind::Material ? record : nullptr;
}

// A material picker: none ("emptyLabel"), or one of the project's materials.
// Returns true with `name` changed when one is picked.
bool materialCombo(const char* label,
                   const std::string& emptyLabel,
                   std::string& name,
                   sfs::MeshLibrary& meshes)
{
  const std::string preview = name.empty() ? emptyLabel : meshes.label(name);
  bool picked = false;
  if (ImGui::BeginCombo(label, preview.c_str()))
  {
    if (ImGui::Selectable(emptyLabel.c_str(), name.empty()))
    {
      name.clear();
      picked = true;
    }
    for (const auto& [option, optionLabel] : meshes.availableMaterials())
    {
      ImGui::PushID(option.c_str());
      if (ImGui::Selectable(optionLabel.c_str(), option == name))
      {
        name = option;
        picked = true;
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  return picked;
}

// A [[sfs::material]] string: pick one of the project's material assets (or
// none), with icons to edit and find it, as a material slot has.
void materialField(const FieldInfo& field,
                   void* object,
                   sfs::MeshLibrary* meshes,
                   EditorHistory* history,
                   const MaterialActions* actions)
{
  std::string& material = field.in<std::string>(object);
  const sfs::AssetRecord* picked =
      actions ? materialAsset(material, sfs::scriptableObjects().catalogue())
              : nullptr;
  const ImGuiStyle& style = ImGui::GetStyle();
  const float iconsWidth =
      picked ? 2.0f * (ImGui::GetFrameHeight() + style.ItemInnerSpacing.x)
             : 0.0f;
  widgets::propertyLabel(field.displayName.c_str());
  ImGui::SetNextItemWidth(std::max(
      ImGui::GetContentRegionAvail().x - iconsWidth, ImGui::GetFrameHeight()));
  std::string chosen = material;
  if (meshes && materialCombo("##value", "None", chosen, *meshes))
  {
    if (history)
      history->change([&] { material = chosen; });
    else
      material = chosen;
  }
  if (picked)
  {
    const std::string file = picked->path.filename().string();
    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    if (widgets::editButton("edit"))
      actions->edit(*picked);
    ImGui::SetItemTooltip("Edit %s", file.c_str());
    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    if (widgets::targetButton("reveal"))
      actions->reveal(*picked);
    ImGui::SetItemTooltip("Show %s in the Assets panel", file.c_str());
  }
}

// One field, with the widget its kind and attributes call for. Every edit is
// recorded for undo.
void fieldWidget(const FieldInfo& field,
                 void* object,
                 sfs::MeshLibrary* meshes,
                 EditorHistory* history,
                 const ScriptActions* scriptActions = nullptr,
                 const MaterialActions* materialActions = nullptr)
{
  const char* label = field.displayName.c_str();
  const auto& attributes = field.attributes;
  using widgets::undoable;
  // The label on the left, the widget (with this hidden id) on the right.
  const char* id = "##value";
  const auto row = [label] { widgets::propertyLabel(label); };

  switch (field.kind)
  {
  case FieldKind::Bool:
    undoable(history,
             field.in<bool>(object),
             [&](bool& v) { return widgets::propertyCheckbox(label, &v); });
    break;
  case FieldKind::Int:
    row();
    undoable(history,
             field.in<int32_t>(object),
             [&](int32_t& v)
             {
               return attributes.hasRange
                          ? ImGui::SliderInt(id,
                                             &v,
                                             static_cast<int>(attributes.min),
                                             static_cast<int>(attributes.max))
                          : ImGui::DragInt(id, &v);
             });
    break;
  case FieldKind::UInt:
    row();
    undoable(history,
             field.in<uint32_t>(object),
             [&](uint32_t& v)
             { return ImGui::DragScalar(id, ImGuiDataType_U32, &v); });
    break;
  case FieldKind::Float:
    row();
    undoable(history,
             field.in<float>(object),
             [&](float& v)
             {
               return attributes.hasRange
                          ? ImGui::SliderFloat(
                                id, &v, attributes.min, attributes.max)
                          : ImGui::DragFloat(id, &v, 0.05f);
             });
    break;
  case FieldKind::String:
    if (attributes.script)
      scriptField(field, object, history, scriptActions);
    else if (attributes.material)
      materialField(field, object, meshes, history, materialActions);
    else if (attributes.texture)
      textureField(field, object, meshes, history);
    else
    {
      row();
      undoable(history,
               field.in<std::string>(object),
               [&](std::string& v) { return widgets::inputText(id, v); });
    }
    break;
  case FieldKind::Vec2:
    row();
    undoable(history,
             field.in<glm::vec2>(object),
             [&](glm::vec2& v) { return ImGui::DragFloat2(id, &v.x, 0.05f); });
    break;
  case FieldKind::Vec3:
    row();
    undoable(history,
             field.in<glm::vec3>(object),
             [&](glm::vec3& v)
             {
               return attributes.color ? widgets::colorEdit3(id, &v.x)
                                       : ImGui::DragFloat3(id, &v.x, 0.05f);
             });
    break;
  case FieldKind::Vec4:
    row();
    undoable(history,
             field.in<glm::vec4>(object),
             [&](glm::vec4& v)
             {
               return attributes.color ? widgets::colorEdit4(id, &v.x)
                                       : ImGui::DragFloat4(id, &v.x, 0.05f);
             });
    break;
  case FieldKind::Quat:
    // Edited as Euler angles in degrees; stored as a quaternion.
    row();
    undoable(history,
             field.in<glm::quat>(object),
             [&](glm::quat& rotation)
             {
               glm::vec3 degrees = glm::degrees(glm::eulerAngles(rotation));
               if (!ImGui::DragFloat3(id, &degrees.x, 0.5f))
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
  case FieldKind::StringList:
    row();
    ImGui::TextDisabled(
        "%zu entries", field.in<std::vector<std::string>>(object).size());
    break;
  case FieldKind::AssetRef:
    undoable(history,
             field.in<sfs::AssetRefBase>(object).asset,
             [&](std::string& asset)
             {
               row();
               return assetRefCombo(id, attributes.assetType, asset);
             });
    break;
  }
}

// A field's value as JSON (a mesh by its handle's id), to compare and copy
// between objects of a type.
nlohmann::json fieldValue(const FieldInfo& field, void* object)
{
  using Kind = FieldKind;
  switch (field.kind)
  {
  case Kind::Bool:
    return field.in<bool>(object);
  case Kind::Int:
    return field.in<int>(object);
  case Kind::UInt:
    return field.in<uint32_t>(object);
  case Kind::Float:
    return field.in<float>(object);
  case Kind::String:
    return field.in<std::string>(object);
  case Kind::Vec2:
  {
    const glm::vec2& v = field.in<glm::vec2>(object);
    return {v.x, v.y};
  }
  case Kind::Vec3:
  {
    const glm::vec3& v = field.in<glm::vec3>(object);
    return {v.x, v.y, v.z};
  }
  case Kind::Vec4:
  {
    const glm::vec4& v = field.in<glm::vec4>(object);
    return {v.x, v.y, v.z, v.w};
  }
  case Kind::Quat:
  {
    const glm::quat& q = field.in<glm::quat>(object);
    return {q.w, q.x, q.y, q.z};
  }
  case Kind::Mesh:
    return field.in<sfs::MeshHandle>(object).id;
  case Kind::Json:
    return field.in<nlohmann::json>(object);
  case Kind::StringList:
    return field.in<std::vector<std::string>>(object);
  case Kind::AssetRef:
    return field.in<sfs::AssetRefBase>(object).asset;
  }
  return nullptr;
}

// Copy a field from one object of a type to another.
void copyField(const FieldInfo& field, void* from, void* to)
{
  using Kind = FieldKind;
  switch (field.kind)
  {
  case Kind::Bool:
    field.in<bool>(to) = field.in<bool>(from);
    break;
  case Kind::Int:
    field.in<int>(to) = field.in<int>(from);
    break;
  case Kind::UInt:
    field.in<uint32_t>(to) = field.in<uint32_t>(from);
    break;
  case Kind::Float:
    field.in<float>(to) = field.in<float>(from);
    break;
  case Kind::String:
    field.in<std::string>(to) = field.in<std::string>(from);
    break;
  case Kind::Vec2:
    field.in<glm::vec2>(to) = field.in<glm::vec2>(from);
    break;
  case Kind::Vec3:
    field.in<glm::vec3>(to) = field.in<glm::vec3>(from);
    break;
  case Kind::Vec4:
    field.in<glm::vec4>(to) = field.in<glm::vec4>(from);
    break;
  case Kind::Quat:
    field.in<glm::quat>(to) = field.in<glm::quat>(from);
    break;
  case Kind::Mesh:
    field.in<sfs::MeshHandle>(to) = field.in<sfs::MeshHandle>(from);
    break;
  case Kind::Json:
    field.in<nlohmann::json>(to) = field.in<nlohmann::json>(from);
    break;
  case Kind::StringList:
    field.in<std::vector<std::string>>(to) =
        field.in<std::vector<std::string>>(from);
    break;
  case Kind::AssetRef:
    field.in<sfs::AssetRefBase>(to) = field.in<sfs::AssetRefBase>(from);
    break;
  }
}

// The attached script's own fields: a game object script's reflected fields,
// with this entity's values (kept in ScriptComponent::fields). Edited through a
// scratch instance of the script, so its widgets are the components' ones.
void scriptFields(sfs::ScriptComponent& component,
                  sfs::MeshLibrary* meshes,
                  EditorHistory* history,
                  std::span<sfs::ScriptComponent* const> others = {})
{
  if (component.script.empty())
    return;
  const TypeInfo* type = sfs::reflect::types().find(component.script);
  const sfs::ScriptType* script = sfs::scripts().find(component.script);
  if (!type || !script || !script->createObject)
  {
    ImGui::TextDisabled(
        "Build the scripts (Ctrl+B) to edit this script's fields");
    return;
  }
  if (type->fields.empty())
    return;

  // The class's defaults, overridden by what this entity has saved.
  const std::unique_ptr<sfs::GameObject> object = script->createObject();
  const sfs::SceneSerializer serializer(sfs::reflect::types(), meshes);
  serializer.loadObject(*type, object.get(), component.fields);
  nlohmann::json current = serializer.saveObject(*type, object.get());
  // Several selected (all running it): theirs, the same way.
  std::vector<std::unique_ptr<sfs::GameObject>> otherObjects;
  std::vector<nlohmann::json> otherValues;
  for (const sfs::ScriptComponent* other : others)
  {
    otherObjects.push_back(script->createObject());
    serializer.loadObject(*type, otherObjects.back().get(), other->fields);
    otherValues.push_back(serializer.saveObject(*type, otherObjects.back().get()));
  }

  ImGui::SeparatorText(type->displayName.c_str());
  for (const FieldInfo& field : type->fields)
  {
    ImGui::PushID(field.name.c_str());
    // Dimmed while theirs differ (see componentSection()).
    const bool mixed = std::any_of(otherValues.begin(),
                                   otherValues.end(),
                                   [&](const nlohmann::json& values)
                                   {
                                     return values.value(field.name, nlohmann::json()) !=
                                            current.value(field.name, nlohmann::json());
                                   });
    if (mixed)
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.55f);
    ImGui::BeginGroup();
    fieldWidget(field, object.get(), meshes, nullptr);
    ImGui::EndGroup();
    if (mixed)
    {
      ImGui::PopStyleVar();
      ImGui::SetItemTooltip("Differs between the selected objects: "
                            "editing it sets it on them all");
    }

    // Bracketed like widgets::undoable: a drag is one undo step.
    const nlohmann::json edited = serializer.saveObject(*type, object.get());
    const bool changed = edited != current;
    if (history && (ImGui::IsItemActivated() || changed))
      history->beginChange();
    if (changed)
    {
      component.fields = edited;
      current = edited;
      // The same value on every other one running it.
      for (std::size_t i = 0; i < others.size(); ++i)
      {
        copyField(field, object.get(), otherObjects[i].get());
        others[i]->fields = serializer.saveObject(*type, otherObjects[i].get());
        otherValues[i] = others[i]->fields;
      }
    }
    if (history &&
        (ImGui::IsItemDeactivated() || (changed && !ImGui::IsItemActive())))
      history->endChange();
    ImGui::PopID();
  }
}

// A [[sfs::materials]] list: a material picker for each section (material
// slot) of the component's mesh, and Extract to turn a mesh's own material
// into a material asset that can be edited.
void materialsField(const TypeInfo& type,
                    const FieldInfo& field,
                    void* object,
                    sfs::MeshLibrary* meshes,
                    sfs::AssetDatabase* assets,
                    EditorHistory* history,
                    const MaterialActions* actions)
{
  ImGui::SeparatorText(field.displayName.c_str());
  const auto meshField = std::find_if(type.fields.begin(),
                                      type.fields.end(),
                                      [](const FieldInfo& f)
                                      { return f.kind == FieldKind::Mesh; });
  const sfs::MeshHandle mesh = meshField != type.fields.end()
                                   ? meshField->in<sfs::MeshHandle>(object)
                                   : sfs::MeshHandle{};
  const std::vector<sfs::MeshSection> slots =
      meshes ? meshes->sections(mesh) : std::vector<sfs::MeshSection>{};
  if (slots.empty())
  {
    ImGui::TextDisabled("Pick a mesh to give it materials");
    return;
  }

  std::vector<std::string>& list = field.in<std::vector<std::string>>(object);
  const auto assign = [&](std::size_t slot, const std::string& name)
  {
    const auto apply = [&]
    {
      if (list.size() <= slot)
        list.resize(slot + 1);
      list[slot] = name;
      while (!list.empty() && list.back().empty())
        list.pop_back(); // trailing "own" slots aren't saved
    };
    if (history)
      history->change(apply);
    else
      apply();
  };

  // "models/barn.glb : Roof" -> "barn", to name extracted materials.
  const std::string meshLabel = meshes->label(meshes->nameOf(mesh));
  const std::string modelName =
      std::filesystem::path(meshLabel.substr(0, meshLabel.find(" : ")))
          .stem()
          .string();

  for (std::size_t slot = 0; slot < slots.size(); ++slot)
  {
    ImGui::PushID(static_cast<int>(slot));
    const std::string own = slots[slot].name.empty()
                                ? "Material " + std::to_string(slot + 1)
                                : slots[slot].name;
    std::string name = slot < list.size() ? list[slot] : std::string();
    // The label, the picker, then (with a material asset picked) icons at
    // the right edge to edit it and find it, as a script field lays them out.
    // The label is the slot: its own material's name, which reads as the
    // material only while that's what it uses.
    const sfs::AssetRecord* picked =
        actions ? materialAsset(name, assets) : nullptr;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float iconsWidth =
        picked ? 2.0f * (ImGui::GetFrameHeight() + style.ItemInnerSpacing.x)
               : 0.0f;
    const std::string slotLabel = name.empty() ? own : "in place of " + own;
    widgets::propertyLabel(slotLabel.c_str());
    if (!name.empty())
      ImGui::SetItemTooltip(
          "This slot's own material (from the mesh) is %s", own.c_str());
    ImGui::SetNextItemWidth(
        std::max(ImGui::GetContentRegionAvail().x - iconsWidth,
                 ImGui::GetFrameHeight()));
    if (materialCombo("##material", "Mesh's own", name, *meshes))
      assign(slot, name);

    // Drop a texture or material from the Assets panel onto the slot.
    if (assets && ImGui::BeginDragDropTarget())
    {
      const sfs::AssetRecord* dragged =
          assetDrop::draggedAsset(ImGui::GetDragDropPayload(), *assets);
      if (dragged && assetDrop::fitsMaterialSlot(*dragged) &&
          ImGui::AcceptDragDropPayload(assetDrop::kAssetPayload))
        if (const std::string material =
                assetDrop::materialFor(*assets, *dragged);
            !material.empty())
          assign(slot, material);
      ImGui::EndDragDropTarget();
    }

    // Extract: rarely needed (to edit a material a model came with), so it
    // lives in the slot's right-click menu.
    if (ImGui::BeginPopupContextItem("slot"))
    {
      if (ImGui::MenuItem("Use Mesh's Own", nullptr, false, !name.empty()))
        assign(slot, {});
      if (ImGui::MenuItem("Extract to Edit", nullptr, false, assets != nullptr))
        if (const auto id = materialFiles::extract(
                *assets, slots[slot].material, modelName + "_" + own))
          assign(slot, "asset:" + id->toString());
      ImGui::SetItemTooltip("Save the mesh's own material, with its textures, "
                            "as a material asset in "
                            "materials/, and use it here");
      ImGui::EndPopup();
    }
    ImGui::SetItemTooltip(
        "Drop a texture or material here. Right-click for more");
    if (picked)
    {
      const std::string file = picked->path.filename().string();
      ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
      if (widgets::editButton("edit"))
        actions->edit(*picked);
      ImGui::SetItemTooltip("Edit %s", file.c_str());
      ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
      if (widgets::targetButton("reveal"))
        actions->reveal(*picked);
      ImGui::SetItemTooltip("Show %s in the Assets panel", file.c_str());
    }
    ImGui::PopID();
  }
}

// Whether a field's [[sfs::showIf(field)]] switch is on (or it has none).
bool fieldShown(const TypeInfo& type, void* object, const FieldInfo& field)
{
  std::string_view name = field.attributes.showIf;
  if (name.empty())
    return true;
  const bool whenOff = name.starts_with('!'); // "!field": while it's off
  if (whenOff)
    name.remove_prefix(1);
  const auto other =
      std::find_if(type.fields.begin(),
                   type.fields.end(),
                   [&](const FieldInfo& f) { return f.name == name; });
  return other == type.fields.end() || other->kind != FieldKind::Bool ||
         other->in<bool>(object) != whenOff;
}

// Whether any of a component's materials can glow (see sfs::canGlow): those
// in its [[sfs::materials]] field's slots, or its mesh's own where a slot has
// none. Its [[sfs::emissionLight]] settings are shown only then.
bool materialsCanGlow(const TypeInfo& type,
                      void* object,
                      sfs::MeshLibrary* meshes)
{
  if (!meshes)
    return true; // can't tell: show them
  const auto meshField = std::find_if(type.fields.begin(),
                                      type.fields.end(),
                                      [](const FieldInfo& f)
                                      { return f.kind == FieldKind::Mesh; });
  const auto materialsField = std::find_if(
      type.fields.begin(),
      type.fields.end(),
      [](const FieldInfo& f)
      { return f.attributes.materials && f.kind == FieldKind::StringList; });
  if (meshField == type.fields.end())
    return true;
  const sfs::MeshHandle mesh = meshField->in<sfs::MeshHandle>(object);
  const std::vector<std::string>* picked =
      materialsField != type.fields.end()
          ? &materialsField->in<std::vector<std::string>>(object)
          : nullptr;
  for (std::size_t slot = 0; slot < meshes->sectionCount(mesh); ++slot)
  {
    const sfs::MeshMaterial* material = nullptr;
    if (picked && slot < picked->size() && !(*picked)[slot].empty())
      material = meshes->materialData(meshes->material((*picked)[slot]));
    if (!material)
      material = meshes->sectionMaterial(mesh, slot);
    if (material && sfs::canGlow(*material))
      return true;
  }
  return false;
}

// What a collider is fitted to: the meshes in the scene.
struct MeshScene
{
  const sfs::MeshLibrary* meshes = nullptr;
  const sfs::hierarchy::EntityTree* tree = nullptr;
  sfs::AssetDatabase* assets = nullptr; // where Extract writes materials
};

// The bounds, in the entity's local space, of its mesh and its descendants'
// (a collider on a group covers the group). Empty if there are none (or only
// stand-ins).
sfs::Aabb meshBounds(const sfs::Entity& entity, const MeshScene& scene)
{
  sfs::Aabb bounds;
  if (!scene.meshes || !scene.tree)
    return bounds;
  const glm::mat4 toLocal = glm::inverse(sfs::hierarchy::worldMatrix(entity));
  for (const sfs::Entity& part : scene.tree->subtree(entity))
  {
    if (!part.hasComponent<sfs::MeshRendererComponent>())
      continue;
    const sfs::Aabb* mesh = scene.meshes->bounds(
        part.getComponent<sfs::MeshRendererComponent>().mesh);
    if (!mesh || mesh->empty())
      continue;
    bounds.expand(
        part == entity
            ? *mesh
            : mesh->transformed(toLocal * sfs::hierarchy::worldMatrix(part)));
  }
  return bounds;
}

bool isCollider(const TypeInfo& type)
{
  return type.name == "BoxCollider3DComponent" ||
         type.name == "SphereCollider3DComponent" ||
         type.name == "CapsuleCollider3DComponent";
}

// Size a collider to the entity's meshes. Does nothing if it isn't a collider
// or there's no mesh to fit.
void fitCollider(const TypeInfo& type,
                 const sfs::Entity& entity,
                 const MeshScene& scene)
{
  const sfs::Aabb bounds =
      isCollider(type) ? meshBounds(entity, scene) : sfs::Aabb();
  if (bounds.empty())
    return;
  void* object = type.component.get(entity);
  const glm::vec3 half = bounds.extents();
  if (type.name == "BoxCollider3DComponent")
  {
    auto& box = *static_cast<sfs::BoxCollider3DComponent*>(object);
    box.size = half * 2.0f;
    box.offset = bounds.center();
    // A flat mesh (a plane) gets a slab under its surface, not a sheet bodies
    // can tunnel through.
    constexpr float kMinThickness = 0.1f;
    for (int axis = 0; axis < 3; ++axis)
      if (box.size[axis] < kMinThickness)
      {
        box.offset[axis] -= (kMinThickness - box.size[axis]) * 0.5f;
        box.size[axis] = kMinThickness;
      }
  }
  else if (type.name == "SphereCollider3DComponent")
  {
    auto& sphere = *static_cast<sfs::SphereCollider3DComponent*>(object);
    sphere.radius = std::max({half.x, half.y, half.z, 0.01f});
    sphere.offset = bounds.center();
  }
  else
  {
    auto& capsule = *static_cast<sfs::CapsuleCollider3DComponent*>(object);
    capsule.radius = std::max({half.x, half.y, 0.01f});
    capsule.height = std::max(half.z * 2.0f, capsule.radius * 2.0f);
    capsule.offset = bounds.center();
  }
}

// A collapsible section for one component, with Remove in its context menu.
// `overridden`: the fields a prefab instance overrides here (marked, with
// Revert to Prefab on right-click, which calls `revert`); null if none.
// `entity`'s component of a type, with every field it shows; with `others`
// (several selected), edits to a field go to all of them, and fields whose
// values differ between them are dimmed.
void componentSection(
    const TypeInfo& type,
    const sfs::Entity& entity,
    sfs::MeshLibrary* meshes,
    const MeshScene& scene,
    EditorHistory* history,
    const std::set<std::string>* overridden = nullptr,
    const std::function<void(const std::string& field)>& revert = {},
    const ScriptActions* scriptActions = nullptr,
    const MaterialActions* materialActions = nullptr,
    std::span<const sfs::Entity> others = {})
{
  ImGui::PushID(type.name.c_str());
  const bool expanded = ImGui::CollapsingHeader(
      type.displayName.c_str(), ImGuiTreeNodeFlags_DefaultOpen);

  bool remove = false;
  if (ImGui::BeginPopupContextItem())
  {
    remove = ImGui::MenuItem("Remove Component");
    ImGui::EndPopup();
  }

  if (remove)
    history->change(
        [&]
        {
          type.component.remove(entity);
          for (const sfs::Entity& other : others)
            type.component.remove(other);
        });
  else if (expanded)
  {
    void* object = type.component.get(entity);
    if (type.name == "TransformComponent")
      if (const sfs::Entity parent = sfs::hierarchy::parentOf(entity);
          parent.isValid())
      {
        const bool named = parent.hasComponent<sfs::NameComponent>();
        ImGui::TextDisabled(
            "Relative to parent \"%s\"",
            named ? parent.getComponent<sfs::NameComponent>().name.c_str()
                  : "(unnamed)");
      }
    const bool glows = materialsCanGlow(type, object, meshes);
    for (const FieldInfo& field : type.fields)
    {
      if (field.attributes.emissionLight && !glows)
        continue; // nothing of it glows: no light to set
      if (!fieldShown(type, object, field))
        continue; // its switch is off
      // Each field in its own ID scope: a label may repeat the header's (the
      // Script component's "Script" field) and must not share its ID.
      ImGui::PushID(field.name.c_str());
      // Several selected: what it was, and whether theirs differ.
      nlohmann::json before;
      bool mixed = false;
      if (!others.empty())
      {
        before = fieldValue(field, object);
        for (const sfs::Entity& other : others)
          mixed = mixed ||
                  fieldValue(field, type.component.get(other)) != before;
      }
      if (mixed)
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                            ImGui::GetStyle().Alpha * 0.55f);
      ImGui::BeginGroup();
      if (field.attributes.materials && field.kind == FieldKind::StringList)
        materialsField(type,
                       field,
                       object,
                       meshes,
                       scene.assets,
                       history,
                       materialActions);
      else
        fieldWidget(
            field, object, meshes, history, scriptActions, materialActions);
      ImGui::EndGroup();
      if (mixed)
      {
        ImGui::PopStyleVar();
        ImGui::SetItemTooltip("Differs between the selected objects: "
                              "editing it sets it on them all");
      }
      // Edited: the same on every selected object, in the same undo step
      // (folded into one just recorded, if the edit made its own).
      if (!others.empty() && fieldValue(field, object) != before)
      {
        for (const sfs::Entity& other : others)
          copyField(field, object, type.component.get(other));
        if (history && !history->changing())
          history->amendLast();
      }
      if (overridden && overridden->contains(field.name))
      {
        // A sage bar at the window's edge: this copy differs from its prefab.
        const float x = ImGui::GetWindowPos().x + 3.0f;
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(x, ImGui::GetItemRectMin().y),
            ImVec2(x + 3.0f, ImGui::GetItemRectMax().y),
            theme::kAccent,
            1.5f);
        if (ImGui::BeginPopupContextItem("override"))
        {
          if (ImGui::MenuItem("Revert to Prefab") && revert)
            revert(field.name);
          ImGui::EndPopup();
        }
        ImGui::SetItemTooltip(
            "Changed on this copy of the prefab (right-click to revert)");
      }
      ImGui::PopID();
    }
    if (type.fields.empty())
      ImGui::TextDisabled("No editable fields");
    if (isCollider(type) && !meshBounds(entity, scene).empty() &&
        ImGui::Button("Fit to Mesh"))
      history->change(
          [&]
          {
            fitCollider(type, entity, scene);
            for (const sfs::Entity& other : others)
              fitCollider(type, other, scene);
          });
    if (type.name == "ScriptComponent")
    {
      // Several selected: their fields too, if they all run the same script.
      auto& script = *static_cast<sfs::ScriptComponent*>(object);
      std::vector<sfs::ScriptComponent*> otherScripts;
      for (const sfs::Entity& other : others)
        otherScripts.push_back(
            static_cast<sfs::ScriptComponent*>(type.component.get(other)));
      if (std::all_of(otherScripts.begin(),
                      otherScripts.end(),
                      [&](const sfs::ScriptComponent* other)
                      { return other->script == script.script; }))
        scriptFields(script, meshes, history, otherScripts);
      else
        ImGui::TextDisabled(
            "They run different scripts: select ones running the same "
            "script to edit its fields together");
    }
  }
  ImGui::PopID();
}

std::string lowercase(std::string text)
{
  for (char& c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// The components that can be added, grouped by the module that registered
// them (the engine's first, then the game's), narrowed by a search box.
// A collider starts out fitted to the entity's meshes.
void addComponentButton(const sfs::Entity& entity,
                        const MeshScene& scene,
                        EditorHistory* history,
                        std::string& filter,
                        std::span<const sfs::Entity> others = {})
{
  if (ImGui::Button("Add Component", ImVec2(-1.0f, 0.0f)))
  {
    filter.clear();
    ImGui::OpenPopup("AddComponent");
  }

  if (!ImGui::BeginPopup("AddComponent"))
    return;

  if (ImGui::IsWindowAppearing())
    ImGui::SetKeyboardFocusHere();
  widgets::searchInput("search", filter, "Search", 220.0f);
  const std::string search = lowercase(filter);

  std::vector<const TypeInfo*> candidates;
  for (const TypeInfo& type : sfs::reflect::types().types())
    if (type.kind == sfs::reflect::TypeKind::Component &&
        type.name != kNameType &&
        (!type.component.has(entity) ||
         std::any_of(others.begin(),
                     others.end(),
                     [&](const sfs::Entity& other)
                     { return !type.component.has(other); })) &&
        lowercase(type.displayName).find(search) != std::string::npos)
      candidates.push_back(&type);
  std::stable_sort(candidates.begin(),
                   candidates.end(),
                   [](const TypeInfo* a, const TypeInfo* b)
                   {
                     const bool aEngine = a->module == "engine";
                     const bool bEngine = b->module == "engine";
                     if (aEngine != bEngine)
                       return aEngine;
                     if (a->module != b->module)
                       return a->module < b->module;
                     return a->displayName < b->displayName;
                   });

  const std::string* section = nullptr;
  for (const TypeInfo* type : candidates)
  {
    if (!section || *section != type->module)
    {
      section = &type->module;
      ImGui::SeparatorText(*section == "engine" ? "Engine" : section->c_str());
    }
    if (ImGui::MenuItem(type->displayName.c_str()))
      history->change(
          [&]
          {
            if (!type->component.has(entity))
            {
              type->component.add(entity);
              fitCollider(*type, entity, scene);
            }
            // To every selected object that hasn't one.
            for (const sfs::Entity& other : others)
              if (!type->component.has(other))
              {
                type->component.add(other);
                fitCollider(*type, other, scene);
              }
          });
  }
  if (candidates.empty())
    ImGui::TextDisabled(search.empty() ? "Every component is already added"
                                       : "No component matches");
  ImGui::EndPopup();
}

} // namespace

namespace
{
// An asset picked in the Assets panel: what it is and its identity.
void assetDetails(const sfs::AssetDatabase& assets,
                  const sfs::AssetRecord& record)
{
  ImGui::SeparatorText(record.path.filename().string().c_str());
  const std::filesystem::path file = assets.absolutePath(record);

  if (ImGui::BeginTable("Asset", 2, ImGuiTableFlags_SizingStretchProp))
  {
    const auto row = [](const char* name, const std::string& value)
    {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%s", name);
      ImGui::TableNextColumn();
      ImGui::TextWrapped("%s", value.c_str());
    };
    row("Kind", sfs::assetKindName(record.kind));
    row("Path", record.path.generic_string());
    std::error_code error;
    const auto bytes = std::filesystem::file_size(file, error);
    row("Size",
        error ? std::string("?")
              : std::to_string((bytes + 1023) / 1024) + " KB");
    row("Id", record.id.valid() ? record.id.toString() : "(none)");
    ImGui::EndTable();
  }

  if (ImGui::Button("Copy Id"))
    ImGui::SetClipboardText(record.id.toString().c_str());
  ImGui::SetItemTooltip(
      "Its id stays the same when it's renamed or moved in the Assets panel");
  ImGui::SameLine();
  // Opened in another app; materials and scriptable objects are edited right
  // here.
  if (record.kind != sfs::AssetKind::Material &&
      record.kind != sfs::AssetKind::ScriptableObject)
  {
    if (ImGui::Button("Open"))
      shell::openExternally(file);
    ImGui::SameLine();
  }
  if (ImGui::Button("Show in Folder"))
    shell::showInFolder(file);

  // A texture's are edited below (textureSettings()).
  if (record.kind == sfs::AssetKind::Texture)
    return;
  ImGui::SeparatorText("Import Settings");
  if (record.settings.empty())
    ImGui::TextDisabled("None");
  else
    ImGui::TextUnformatted(record.settings.dump(2).c_str());
}

// How a texture is brought in: smooth or pixel art, and whether it's a sprite
// sheet (a grid of frames). Saved to its .meta, and shown straight away.
void textureSettings(EditorContext& context, const sfs::AssetRecord& record)
{
  ImGui::SeparatorText("Import Settings");
  nlohmann::json settings =
      record.settings.is_object() ? record.settings : nlohmann::json::object();
  const sfs::TextureImportSettings current =
      sfs::TextureImportSettings::fromJson(settings);
  bool changed = false;

  const char* filters[] = {"Smooth", "Pixel Art"};
  int filter = current.nearest ? 1 : 0;
  widgets::propertyLabel("Filter");
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::Combo("##filter", &filter, filters, 2))
  {
    settings["filter"] = filter == 1 ? "nearest" : "linear";
    changed = true;
  }
  ImGui::SetItemTooltip("Pixel Art keeps its texels crisp when scaled up");

  bool sheet = current.spriteCell.x > 0 && current.spriteCell.y > 0;
  if (widgets::propertyCheckbox("Sprite Sheet", &sheet))
  {
    if (sheet)
      settings["spriteCell"] = {32, 32};
    else
      settings.erase("spriteCell");
    changed = true;
  }
  ImGui::SetItemTooltip(
      "A grid of frames, numbered across then down from the top left;\n"
      "a sprite picks one by its Frame");
  if (sheet)
  {
    glm::ivec2 cell = current.spriteCell;
    widgets::propertyLabel("Frame Size");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputInt2("##cell", &cell.x))
    {
      settings["spriteCell"] = {std::max(cell.x, 1), std::max(cell.y, 1)};
      changed = true;
    }
  }

  if (changed && context.assets->setSettings(record.id, settings))
  {
    if (context.scene)
      context.scene->assetStore().forgetTexture(record.id);
    context.assetFilesChanged = true; // its preview
  }
}
} // namespace

// A texture picker: none, or one of the project's textures. Returns true with
// `name` changed when one is picked.
static bool textureCombo(const char* label,
                         std::string& name,
                         const sfs::MeshLibrary& meshes)
{
  const std::string preview = name.empty() ? "None" : meshes.label(name);
  bool picked = false;
  widgets::propertyLabel(label);
  if (ImGui::BeginCombo((std::string("##") + label).c_str(), preview.c_str()))
  {
    if (ImGui::Selectable("None", name.empty()))
    {
      name.clear();
      picked = true;
    }
    for (const auto& [option, optionLabel] : meshes.availableTextures())
    {
      ImGui::PushID(option.c_str());
      if (ImGui::Selectable(optionLabel.c_str(), option == name))
      {
        name = option;
        picked = true;
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  return picked;
}

void InspectorPanel::drawScriptableObject(EditorContext& context,
                                          const sfs::AssetRecord& record)
{
  // Read once per selection; every edit saves the file, and a playing scene
  // reads it again.
  if (m_scriptableId != record.id)
  {
    m_scriptableId = record.id;
    m_data = scriptableObjectFiles::read(*context.assets, record);
  }
  if (!m_data)
    return;
  const std::string typeName = m_data->value("type", std::string());
  const TypeInfo* type = sfs::reflect::types().find(typeName);
  if (!type || type->kind != sfs::reflect::TypeKind::ScriptableObject)
  {
    ImGui::SeparatorText(typeName.c_str());
    ImGui::TextDisabled(
        "Build the scripts (Ctrl+B) to edit a %s", typeName.c_str());
    return;
  }

  // Edited through a scratch object of the type, so its widgets are the
  // components' ones.
  sfs::MeshLibrary* meshes =
      context.render ? &context.render->meshes() : nullptr;
  const std::unique_ptr<void, void (*)(void*)> object(
      type->scriptable.create(), type->scriptable.destroy);
  const sfs::SceneSerializer serializer(sfs::reflect::types(), meshes);
  const nlohmann::json fields =
      m_data->value("fields", nlohmann::json::object());
  serializer.loadObject(*type, object.get(), fields);
  const nlohmann::json before = serializer.saveObject(*type, object.get());

  ImGui::SeparatorText(type->displayName.c_str());
  for (const FieldInfo& field : type->fields)
  {
    ImGui::PushID(field.name.c_str());
    fieldWidget(field, object.get(), meshes, nullptr);
    ImGui::PopID();
  }
  if (type->fields.empty())
    ImGui::TextDisabled("No editable fields");

  const nlohmann::json after = serializer.saveObject(*type, object.get());
  if (after != before &&
      scriptableObjectFiles::write(*context.assets, record, typeName, after))
  {
    (*m_data)["fields"] = after;
    sfs::scriptableObjects().forget("asset:" + record.id.toString());
  }
}

void InspectorPanel::drawMaterial(EditorContext& context,
                                  const sfs::AssetRecord& record)
{
  // Read once per selection; every edit saves the file and updates the scene.
  if (m_materialId != record.id)
  {
    m_materialId = record.id;
    m_material = materialFiles::read(*context.assets, record);
  }
  ImGui::SeparatorText("Material");
  if (!m_material || !context.render)
  {
    ImGui::TextDisabled(m_material ? "Open a scene to edit materials"
                                   : "Can't read this material");
    return;
  }

  sfs::MaterialAsset& material = *m_material;
  sfs::MeshLibrary& meshes = context.render->meshes();

  // A node-graph material is edited in its own tab.
  if (material.graph)
  {
    ImGui::TextWrapped(
        "A node-graph material: its look comes from its graph of nodes.");
    if (ImGui::Button("Edit Graph"))
      context.openMaterial = record.id;
    ImGui::SetItemTooltip("Open the graph in its own tab (or double-click the "
                          "material in Assets)");
    return;
  }

  bool changed = false;
  // Each row's label on the left, its widget (by this hidden id) on the right.
  const auto row = [](const char* label)
  {
    widgets::propertyLabel(label);
    return std::string("##") + label;
  };
  changed |=
      widgets::colorEdit4(row("Base Colour").c_str(), &material.baseColor.x);
  changed |= textureCombo("Texture", material.baseTexture, meshes);
  changed |=
      ImGui::DragFloat2(row("Tiling").c_str(), &material.tiling.x, 0.05f);
  ImGui::SetItemTooltip(
      "How many times the textures repeat across the surface");
  changed |=
      ImGui::DragFloat2(row("Offset").c_str(), &material.offset.x, 0.01f);
  ImGui::SetItemTooltip("Shifts the textures, in texture widths");
  changed |= ImGui::SliderFloat(
      row("Metallic").c_str(), &material.metallic, 0.0f, 1.0f);
  ImGui::SetItemTooltip("0 for paint, wood, plastic; 1 for bare metal");
  changed |= ImGui::SliderFloat(
      row("Roughness").c_str(), &material.roughness, 0.0f, 1.0f);
  ImGui::SetItemTooltip("0 is mirror-smooth, 1 is matte");
  changed |= textureCombo(
      "Metal/Rough Texture", material.metallicRoughnessTexture, meshes);
  ImGui::SetItemTooltip(
      "Metalness in blue, roughness in green; multiplies the two above");
  changed |= textureCombo("Normal Map", material.normalTexture, meshes);
  ImGui::SetItemTooltip("Bumps and grooves painted as colours (the bluish "
                        "image that comes with many\n"
                        "textures; OpenGL style, green up), so light catches "
                        "detail the mesh doesn't have");
  if (!material.normalTexture.empty())
  {
    changed |= ImGui::DragFloat(row("Bump Strength").c_str(),
                                &material.normalScale,
                                0.01f,
                                0.0f,
                                10.0f);
    ImGui::SetItemTooltip(
        "How deep the normal map's bumps look: 0 is flat, above 1 deeper");
  }

  const char* modes[] = {"Opaque",
                         "Cut-out (mask)",
                         "See-through (blend)",
                         "Glow (additive)"};
  int mode = static_cast<int>(material.alphaMode);
  if (ImGui::Combo(row("Transparency").c_str(), &mode, modes, 4))
  {
    material.alphaMode = static_cast<sfs::AlphaMode>(mode);
    changed = true;
  }
  if (material.alphaMode == sfs::AlphaMode::Mask)
    changed |= ImGui::SliderFloat(
        row("Cutoff").c_str(), &material.alphaCutoff, 0.0f, 1.0f);
  else if (sfs::isSeeThrough(material.alphaMode))
    ImGui::TextDisabled(
        "Opacity is the base colour's alpha, times the texture's");

  // Or work it out with nodes: the same look to start from.
  ImGui::Spacing();
  if (ImGui::Button("Convert to Node Graph"))
  {
    material.graph = material.toGraph();
    changed = true;
    context.openMaterial = record.id;
  }
  ImGui::SetItemTooltip(
      "Make this material a node graph, to build its look from nodes (noise, "
      "masks,\n"
      "glow, ...). It looks the same until you change the graph.");

  if (changed && materialFiles::write(*context.assets, record, material))
  {
    context.assetFilesChanged = true; // its preview
    const std::string name = "asset:" + record.id.toString();
    context.scene->assetStore().forgetMaterial(record.id);
    meshes.reloadMaterial(name);
  }
}

void InspectorPanel::drawContents(EditorContext& context)
{
  // An entity selection wins over an asset one (and replaces it).
  if (context.hasSelection())
    context.selectedAsset = {};
  else
  {
    const sfs::AssetRecord* asset =
        context.assets && context.selectedAsset.valid()
            ? context.assets->find(context.selectedAsset)
            : nullptr;
    if (asset)
    {
      assetDetails(*context.assets, *asset);
      if (asset->kind == sfs::AssetKind::Texture)
        textureSettings(context, *asset);
      if (asset->kind == sfs::AssetKind::Material)
        drawMaterial(context, *asset);
      if (asset->kind == sfs::AssetKind::ScriptableObject)
        drawScriptableObject(context, *asset);
      if (asset->kind == sfs::AssetKind::Prefab && ImGui::Button("Edit Prefab"))
        context.openPrefab = asset->id;
    }
    else
      ImGui::TextDisabled("Nothing selected");
    return;
  }

  // A copy: buttons below may change the selection (Select Asset clears it).
  const sfs::Entity entity = context.selection;
  sfs::MeshLibrary* meshes =
      context.render ? &context.render->meshes() : nullptr;
  const sfs::hierarchy::EntityTree tree(context.scene->entities());
  const MeshScene scene{meshes, &tree, context.assets};

  EditorHistory* history = context.history;

  // Several selected: what they share, edited on them all at once.
  const std::vector<sfs::Entity> selected = context.selected();
  const std::span<const sfs::Entity> others =
      std::span<const sfs::Entity>(selected).subspan(1);
  if (!others.empty())
  {
    ImGui::Text("%zu objects selected", selected.size());
    ImGui::TextDisabled("The components they all have: edits go to every one");
    ImGui::Separator();
  }
  else
  {
    if (!entity.hasComponent<sfs::NameComponent>())
      sfs::Entity(entity).addComponent<sfs::NameComponent>();
    widgets::undoable(history,
                      entity.getComponent<sfs::NameComponent>().name,
                      [](std::string& name)
                      {
                        widgets::propertyLabel("Name");
                        return widgets::inputText("##name", name);
                      });
    ImGui::Separator();

    drawPrefabHeader(context, entity);
  }

  // A script's icons: edit it in the code panel, or find it in Assets.
  const auto withScriptFile = [&](const std::string& script,
                                  std::filesystem::path EditorContext::*request)
  {
    if (!context.project)
      return;
    if (const auto file = scriptFiles::find(context.project->scriptsFolder(),
                                            script,
                                            scriptFiles::Prefer::Source))
      context.*request = *file;
    else
      LOG_ERROR("Can't find " + script + ".cpp or " + script +
                ".h in the scripts folder");
  };
  const ScriptActions scriptActions{
      .edit = [&](const std::string& script)
      { withScriptFile(script, &EditorContext::openText); },
      .reveal = [&](const std::string& script)
      { withScriptFile(script, &EditorContext::revealFile); }};

  // A node-graph material opens in the node editor; a standard one, which
  // the Inspector edits, is selected so it shows there (with Convert to Node
  // Graph). Finding one shows it highlighted in the Assets panel, as a
  // script's is.
  const MaterialActions materialActions{
      .edit =
          [&](const sfs::AssetRecord& material)
      {
        const std::optional<sfs::MaterialAsset> asset =
            context.assets ? materialFiles::read(*context.assets, material)
                           : std::nullopt;
        if (asset && asset->graph)
          context.openMaterial = material.id;
        else
        {
          context.selectedAsset = material.id;
          context.selection = {};
        }
      },
      .reveal =
          [&](const sfs::AssetRecord& material)
      {
        if (context.assets)
          context.revealFile = context.assets->absolutePath(material);
      }};

  const auto overrides = prefabs::overridesOf(context, entity);
  for (const TypeInfo& type : sfs::reflect::types().types())
    if (type.kind == sfs::reflect::TypeKind::Component &&
        type.name != kNameType && type.component.has(entity) &&
        std::all_of(others.begin(),
                    others.end(),
                    [&](const sfs::Entity& other)
                    { return type.component.has(other); }))
    {
      const auto it = overrides.find(type.name);
      componentSection(
          type,
          entity,
          meshes,
          scene,
          history,
          it != overrides.end() ? &it->second : nullptr,
          [&](const std::string& field) {
            m_pendingRevert = {type.name, field};
          },
          &scriptActions,
          &materialActions,
          others);
    }

  ImGui::Spacing();
  addComponentButton(entity, scene, history, m_componentFilter, others);

  // Prefab actions rebuild entities: after drawing, not mid-way.
  if (m_pendingRevert)
    prefabs::revertField(
        context, entity, m_pendingRevert->first, m_pendingRevert->second);
  m_pendingRevert.reset();
  if (m_pendingInstanceAction)
  {
    const sfs::Entity root = sfs::SceneSerializer::instanceRootOf(entity);
    if (*m_pendingInstanceAction == InstanceAction::Apply)
      prefabs::apply(context, root);
    else
      prefabs::revert(context, root);
  }
  m_pendingInstanceAction.reset();
}

void InspectorPanel::drawPrefabHeader(EditorContext& context,
                                      const sfs::Entity& entity)
{
  if (entity.hasComponent<sfs::MissingPrefabComponent>())
  {
    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "Missing prefab");
    ImGui::TextWrapped("%s",
                       entity.getComponent<sfs::MissingPrefabComponent>()
                           .entry.value("$prefab", "")
                           .c_str());
    ImGui::TextDisabled("Kept as saved until its prefab is back");
    ImGui::Separator();
    return;
  }
  const sfs::Entity root = sfs::SceneSerializer::instanceRootOf(entity);
  if (!root.isValid())
    return;
  const std::string name =
      root.getComponent<sfs::PrefabInstanceComponent>().prefab;
  const sfs::AssetRecord* record = prefabs::recordOf(context, name);

  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kAccent),
                     entity == root ? "Prefab" : "Part of prefab");
  ImGui::SameLine();
  ImGui::TextUnformatted(prefabs::label(context, name).c_str());
  if (entity != root)
  {
    if (ImGui::SmallButton("Select Instance"))
      context.selection = root;
    ImGui::Separator();
    return;
  }
  const bool canApply = record && record->kind == sfs::AssetKind::Prefab;
  ImGui::BeginDisabled(!canApply);
  if (ImGui::SmallButton("Edit"))
    context.openPrefab = record->id;
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("Edit the prefab in its own tab");
  ImGui::SameLine();
  if (record && ImGui::SmallButton("Select Asset"))
  {
    context.selectedAsset = record->id;
    context.revealAsset = record->id;
    context.selection = {};
  }
  ImGui::SetItemTooltip("Show the prefab in the Assets panel");
  ImGui::SameLine();
  ImGui::BeginDisabled(!canApply);
  if (ImGui::SmallButton("Apply"))
    m_pendingInstanceAction = InstanceAction::Apply;
  ImGui::EndDisabled();
  ImGui::SetItemTooltip(
      canApply ? "Write this copy's changes into the prefab: every copy updates"
               : "A model's own content can't be changed; make a prefab of it "
                 "first");
  ImGui::SameLine();
  if (ImGui::SmallButton("Revert"))
    m_pendingInstanceAction = InstanceAction::Revert;
  ImGui::SetItemTooltip("Drop this copy's changes (it stays where it is)");
  ImGui::Separator();
}
