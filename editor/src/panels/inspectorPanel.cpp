#include "panels/inspectorPanel.h"

#include "editorContext.h"
#include "assets/assetDrop.h"
#include "assets/materialFiles.h"
#include "platform/shell.h"
#include "scene/editorScene.h"
#include "scene/prefabs.h"
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
#include "engine/core/reflection/reflection.h"
#include "engine/core/rendering/3d/aabb.h"
#include "engine/runtime/rendering3d/meshLibrary.h"
#include "engine/runtime/rendering3d/renderSystem3D.h"
#include "engine/runtime/game/gameObject.h"
#include "engine/runtime/scripting/scriptRegistry.h"
#include "engine/runtime/serialization/prefabComponents.h"
#include "engine/runtime/serialization/sceneSerializer.h"
#include "glm/glm/gtc/quaternion.hpp"
#include "glm/glm/matrix.hpp"
#include "glm/glm/trigonometric.hpp"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <set>
#include <functional>
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
  std::string current =
      meshes && mesh.valid() ? meshes->label(meshes->nameOf(mesh)) : std::string();
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
        if (const sfs::MeshHandle loaded = meshes->resolve(name); loaded.valid())
          choose(loaded);
      ImGui::PopID();
    }
  }
  ImGui::EndCombo();
}

// A [[sfs::texture]] string: pick one of the project's textures (or none:
// the mesh's own).
void textureField(const FieldInfo& field, void* object, sfs::MeshLibrary* meshes, EditorHistory* history)
{
  std::string& texture = field.in<std::string>(object);
  const std::string current = texture.empty() ? "Mesh's own" : meshes ? meshes->label(texture) : texture;

  if (!ImGui::BeginCombo(field.displayName.c_str(), current.c_str()))
    return;
  const auto choose = [&](const std::string& chosen)
  {
    if (history)
      history->change([&] { texture = chosen; });
    else
      texture = chosen;
  };
  if (ImGui::Selectable("Mesh's own", texture.empty()))
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
    else if (attributes.texture)
      textureField(field, object, meshes, history);
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
      return attributes.color ? widgets::colorEdit3(label, &v.x)
                              : ImGui::DragFloat3(label, &v.x, 0.05f);
    });
    break;
  case FieldKind::Vec4:
    undoable(history, field.in<glm::vec4>(object), [&](glm::vec4& v) {
      return attributes.color ? widgets::colorEdit4(label, &v.x)
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
  case FieldKind::StringList:
    ImGui::TextDisabled("%s: %zu entries", label, field.in<std::vector<std::string>>(object).size());
    break;
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

// A material picker: none ("emptyLabel"), or one of the project's materials.
// Returns true with `name` changed when one is picked.
bool materialCombo(const char* label, const std::string& emptyLabel, std::string& name, sfs::MeshLibrary& meshes)
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

// A [[sfs::materials]] list: a material picker for each section (material
// slot) of the component's mesh, and Extract to turn a mesh's own material
// into a material asset that can be edited.
void materialsField(const TypeInfo& type,
                    const FieldInfo& field,
                    void* object,
                    sfs::MeshLibrary* meshes,
                    sfs::AssetDatabase* assets,
                    EditorHistory* history)
{
  ImGui::SeparatorText(field.displayName.c_str());
  const auto meshField = std::find_if(type.fields.begin(),
                                      type.fields.end(),
                                      [](const FieldInfo& f) { return f.kind == FieldKind::Mesh; });
  const sfs::MeshHandle mesh = meshField != type.fields.end() ? meshField->in<sfs::MeshHandle>(object)
                                                              : sfs::MeshHandle{};
  const std::vector<sfs::MeshSection> slots = meshes ? meshes->sections(mesh) : std::vector<sfs::MeshSection>{};
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
      std::filesystem::path(meshLabel.substr(0, meshLabel.find(" : "))).stem().string();

  for (std::size_t slot = 0; slot < slots.size(); ++slot)
  {
    ImGui::PushID(static_cast<int>(slot));
    const std::string own = slots[slot].name.empty() ? "Material " + std::to_string(slot + 1) : slots[slot].name;
    std::string name = slot < list.size() ? list[slot] : std::string();
    if (materialCombo("##material", "Mesh's own", name, *meshes))
      assign(slot, name);

    // Drop a texture or material from the Assets panel onto the slot.
    if (assets && ImGui::BeginDragDropTarget())
    {
      const sfs::AssetRecord* dragged = assetDrop::draggedAsset(ImGui::GetDragDropPayload(), *assets);
      if (dragged && assetDrop::fitsMaterialSlot(*dragged) &&
          ImGui::AcceptDragDropPayload(assetDrop::kAssetPayload))
        if (const std::string material = assetDrop::materialFor(*assets, *dragged); !material.empty())
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
        if (const auto id = materialFiles::extract(*assets, slots[slot].material, modelName + "_" + own))
          assign(slot, "asset:" + id->toString());
      ImGui::SetItemTooltip("Save the mesh's own material, with its textures, as a material asset in "
                            "materials/, and use it here");
      ImGui::EndPopup();
    }
    ImGui::SetItemTooltip("Drop a texture or material here. Right-click for more");
    ImGui::SameLine();
    ImGui::TextUnformatted(own.c_str());
    ImGui::PopID();
  }
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
    const sfs::Aabb* mesh = scene.meshes->bounds(part.getComponent<sfs::MeshRendererComponent>().mesh);
    if (!mesh || mesh->empty())
      continue;
    bounds.expand(part == entity ? *mesh
                                 : mesh->transformed(toLocal * sfs::hierarchy::worldMatrix(part)));
  }
  return bounds;
}

bool isCollider(const TypeInfo& type)
{
  return type.name == "BoxCollider3DComponent" || type.name == "SphereCollider3DComponent" ||
         type.name == "CapsuleCollider3DComponent";
}

// Size a collider to the entity's meshes. Does nothing if it isn't a collider or
// there's no mesh to fit.
void fitCollider(const TypeInfo& type, const sfs::Entity& entity, const MeshScene& scene)
{
  const sfs::Aabb bounds = isCollider(type) ? meshBounds(entity, scene) : sfs::Aabb();
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
void componentSection(const TypeInfo& type,
                      const sfs::Entity& entity,
                      sfs::MeshLibrary* meshes,
                      const MeshScene& scene,
                      EditorHistory* history,
                      const std::set<std::string>* overridden = nullptr,
                      const std::function<void(const std::string& field)>& revert = {})
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
    if (type.name == "Transform3DComponent")
      if (const sfs::Entity parent = sfs::hierarchy::parentOf(entity); parent.isValid())
      {
        const bool named = parent.hasComponent<sfs::NameComponent>();
        ImGui::TextDisabled("Relative to parent \"%s\"",
                            named ? parent.getComponent<sfs::NameComponent>().name.c_str()
                                  : "(unnamed)");
      }
    for (const FieldInfo& field : type.fields)
    {
      // Each field in its own ID scope: a label may repeat the header's (the
      // Script component's "Script" field) and must not share its ID.
      ImGui::PushID(field.name.c_str());
      ImGui::BeginGroup();
      if (field.attributes.materials && field.kind == FieldKind::StringList)
        materialsField(type, field, object, meshes, scene.assets, history);
      else
        fieldWidget(field, object, meshes, history);
      ImGui::EndGroup();
      if (overridden && overridden->contains(field.name))
      {
        // A sage bar at the window's edge: this copy differs from its prefab.
        const float x = ImGui::GetWindowPos().x + 3.0f;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x, ImGui::GetItemRectMin().y),
                                                  ImVec2(x + 3.0f, ImGui::GetItemRectMax().y),
                                                  theme::kAccent,
                                                  1.5f);
        if (ImGui::BeginPopupContextItem("override"))
        {
          if (ImGui::MenuItem("Revert to Prefab") && revert)
            revert(field.name);
          ImGui::EndPopup();
        }
        ImGui::SetItemTooltip("Changed on this copy of the prefab (right-click to revert)");
      }
      ImGui::PopID();
    }
    if (type.fields.empty())
      ImGui::TextDisabled("No editable fields");
    if (isCollider(type) && !meshBounds(entity, scene).empty() && ImGui::Button("Fit to Mesh"))
      history->change([&] { fitCollider(type, entity, scene); });
    if (type.name == "ScriptComponent")
      scriptFields(*static_cast<sfs::ScriptComponent*>(object), meshes, history);
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
                        std::string& filter)
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
    if (type.kind == sfs::reflect::TypeKind::Component && type.name != kNameType &&
        !type.component.has(entity) &&
        lowercase(type.displayName).find(search) != std::string::npos)
      candidates.push_back(&type);
  std::stable_sort(candidates.begin(), candidates.end(),
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
            type->component.add(entity);
            fitCollider(*type, entity, scene);
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
void assetDetails(const sfs::AssetDatabase& assets, const sfs::AssetRecord& record)
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
    row("Size", error ? std::string("?") : std::to_string((bytes + 1023) / 1024) + " KB");
    row("Id", record.id.valid() ? record.id.toString() : "(none)");
    ImGui::EndTable();
  }

  if (ImGui::Button("Copy Id"))
    ImGui::SetClipboardText(record.id.toString().c_str());
  ImGui::SetItemTooltip("Its id stays the same when it's renamed or moved in the Assets panel");
  ImGui::SameLine();
  if (ImGui::Button("Open"))
    shell::openExternally(file);
  ImGui::SameLine();
  if (ImGui::Button("Show in Folder"))
    shell::openExternally(file.parent_path());

  ImGui::SeparatorText("Import Settings");
  if (record.settings.empty())
    ImGui::TextDisabled("None");
  else
    ImGui::TextUnformatted(record.settings.dump(2).c_str());
}
} // namespace

// A texture picker: none, or one of the project's textures. Returns true with
// `name` changed when one is picked.
static bool textureCombo(const char* label, std::string& name, const sfs::MeshLibrary& meshes)
{
  const std::string preview = name.empty() ? "None" : meshes.label(name);
  bool picked = false;
  if (ImGui::BeginCombo(label, preview.c_str()))
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

void InspectorPanel::drawMaterial(EditorContext& context, const sfs::AssetRecord& record)
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
    ImGui::TextDisabled(m_material ? "Open a scene to edit materials" : "Can't read this material");
    return;
  }

  sfs::MaterialAsset& material = *m_material;
  sfs::MeshLibrary& meshes = context.render->meshes();
  bool changed = false;
  changed |= widgets::colorEdit4("Base Colour", &material.baseColor.x);
  changed |= textureCombo("Texture", material.baseTexture, meshes);
  changed |= ImGui::DragFloat2("Tiling", &material.tiling.x, 0.05f);
  ImGui::SetItemTooltip("How many times the textures repeat across the surface");
  changed |= ImGui::DragFloat2("Offset", &material.offset.x, 0.01f);
  ImGui::SetItemTooltip("Shifts the textures, in texture widths");
  changed |= ImGui::SliderFloat("Metallic", &material.metallic, 0.0f, 1.0f);
  ImGui::SetItemTooltip("0 for paint, wood, plastic; 1 for bare metal");
  changed |= ImGui::SliderFloat("Roughness", &material.roughness, 0.0f, 1.0f);
  ImGui::SetItemTooltip("0 is mirror-smooth, 1 is matte");
  changed |= textureCombo("Metal/Rough Texture", material.metallicRoughnessTexture, meshes);
  ImGui::SetItemTooltip("Metalness in blue, roughness in green; multiplies the two above");

  const char* modes[] = {"Opaque", "Cut-out (mask)", "See-through (blend)"};
  int mode = static_cast<int>(material.alphaMode);
  if (ImGui::Combo("Transparency", &mode, modes, 3))
  {
    material.alphaMode = static_cast<sfs::AlphaMode>(mode);
    changed = true;
  }
  if (material.alphaMode == sfs::AlphaMode::Mask)
    changed |= ImGui::SliderFloat("Cutoff", &material.alphaCutoff, 0.0f, 1.0f);
  else if (material.alphaMode == sfs::AlphaMode::Blend)
    ImGui::TextDisabled("Opacity is the base colour's alpha, times the texture's");

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
    const sfs::AssetRecord* asset = context.assets && context.selectedAsset.valid()
                                        ? context.assets->find(context.selectedAsset)
                                        : nullptr;
    if (asset)
    {
      assetDetails(*context.assets, *asset);
      if (asset->kind == sfs::AssetKind::Material)
        drawMaterial(context, *asset);
      if (asset->kind == sfs::AssetKind::Prefab && ImGui::Button("Edit Prefab"))
        context.openPrefab = asset->id;
    }
    else
      ImGui::TextDisabled("Nothing selected");
    return;
  }

  // A copy: buttons below may change the selection (Select Asset clears it).
  const sfs::Entity entity = context.selection;
  sfs::MeshLibrary* meshes = context.render ? &context.render->meshes() : nullptr;
  const sfs::hierarchy::EntityTree tree(context.scene->entities());
  const MeshScene scene{meshes, &tree, context.assets};

  EditorHistory* history = context.history;

  if (!entity.hasComponent<sfs::NameComponent>())
    sfs::Entity(entity).addComponent<sfs::NameComponent>();
  widgets::undoable(history, entity.getComponent<sfs::NameComponent>().name,
                    [](std::string& name) { return widgets::inputText("Name", name); });
  ImGui::Separator();

  drawPrefabHeader(context, entity);

  const auto overrides = prefabs::overridesOf(context, entity);
  for (const TypeInfo& type : sfs::reflect::types().types())
    if (type.kind == sfs::reflect::TypeKind::Component && type.name != kNameType &&
        type.component.has(entity))
    {
      const auto it = overrides.find(type.name);
      componentSection(type, entity, meshes, scene, history, it != overrides.end() ? &it->second : nullptr,
                       [&](const std::string& field) { m_pendingRevert = {type.name, field}; });
    }

  ImGui::Spacing();
  addComponentButton(entity, scene, history, m_componentFilter);

  // Prefab actions rebuild entities: after drawing, not mid-way.
  if (m_pendingRevert)
    prefabs::revertField(context, entity, m_pendingRevert->first, m_pendingRevert->second);
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

void InspectorPanel::drawPrefabHeader(EditorContext& context, const sfs::Entity& entity)
{
  if (entity.hasComponent<sfs::MissingPrefabComponent>())
  {
    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "Missing prefab");
    ImGui::TextWrapped("%s", entity.getComponent<sfs::MissingPrefabComponent>().entry.value("$prefab", "").c_str());
    ImGui::TextDisabled("Kept as saved until its prefab is back");
    ImGui::Separator();
    return;
  }
  const sfs::Entity root = sfs::SceneSerializer::instanceRootOf(entity);
  if (!root.isValid())
    return;
  const std::string name = root.getComponent<sfs::PrefabInstanceComponent>().prefab;
  const sfs::AssetRecord* record = prefabs::recordOf(context, name);

  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kAccent), entity == root ? "Prefab" : "Part of prefab");
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
  ImGui::SetItemTooltip(canApply ? "Write this copy's changes into the prefab: every copy updates"
                                 : "A model's own content can't be changed; make a prefab of it first");
  ImGui::SameLine();
  if (ImGui::SmallButton("Revert"))
    m_pendingInstanceAction = InstanceAction::Revert;
  ImGui::SetItemTooltip("Drop this copy's changes (it stays where it is)");
  ImGui::Separator();
}
