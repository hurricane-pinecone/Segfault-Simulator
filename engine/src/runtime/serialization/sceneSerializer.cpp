#include "engine/runtime/serialization/sceneSerializer.h"

#include "engine/core/assets/scriptableObject.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/logger/logger.h"
#include "engine/core/reflection/reflection.h"
#include "engine/core/util/atomicFile.h"
#include "engine/runtime/rendering/meshLibrary.h"
#include "engine/runtime/rendering/renderSystem.h"
#include "engine/runtime/sceneManager/scene.h"
#include "engine/runtime/serialization/legacyComponentNames.h"
#include "engine/runtime/serialization/prefabComponents.h"
#include "engine/runtime/serialization/unresolvedComponents.h"
#include "glm/glm/ext/quaternion_float.hpp"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>

using nlohmann::json;

namespace sfs
{

namespace
{

// Bumped when the file layout changes incompatibly. 2: hierarchies (nested
// "children"); format 1 files still load.
constexpr int kFormatVersion = 2;

// The key an entity's children are nested under, beside its components.
constexpr const char* kChildrenKey = "children";

// A float as the double with the same shortest decimal spelling, so 0.45f is
// saved as 0.45 rather than its widened value 0.44999998807907104.
double cleanFloat(float value)
{
  char buffer[32];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  return std::strtod(std::string(buffer, result.ptr).c_str(), nullptr);
}

// --- glm <-> JSON arrays -----------------------------------------------------

template <int N>
json vecToJson(const float* values)
{
  json array = json::array();
  for (int i = 0; i < N; ++i)
    array.push_back(cleanFloat(values[i]));
  return array;
}

template <int N>
bool vecFromJson(const json& value, float* out)
{
  if (!value.is_array() || value.size() != N)
    return false;
  for (int i = 0; i < N; ++i)
  {
    if (!value[i].is_number())
      return false;
    out[i] = value[i].get<float>();
  }
  return true;
}

json toJson(const glm::vec2& v) { return vecToJson<2>(&v.x); }
json toJson(const glm::vec3& v) { return vecToJson<3>(&v.x); }

glm::vec2 vec2Or(const json& object, const char* key, glm::vec2 fallback)
{
  if (object.contains(key))
    vecFromJson<2>(object[key], &fallback.x);
  return fallback;
}

glm::vec3 vec3Or(const json& object, const char* key, glm::vec3 fallback)
{
  if (object.contains(key))
    vecFromJson<3>(object[key], &fallback.x);
  return fallback;
}

template <typename T>
T valueOr(const json& object, const char* key, T fallback)
{
  if (object.contains(key) && !object[key].is_null())
  {
    try
    {
      return object[key].get<T>();
    }
    catch (const json::exception&)
    {
    }
  }
  return fallback;
}

// --- reflected fields
// ----------------------------------------------------------

json fieldToJson(const reflect::FieldInfo& field,
                 void* object,
                 const MeshLibrary* meshes)
{
  using Kind = reflect::FieldKind;
  switch (field.kind)
  {
  case Kind::Bool:
    return field.in<bool>(object);
  case Kind::Int:
    return field.in<int32_t>(object);
  case Kind::UInt:
    return field.in<uint32_t>(object);
  case Kind::Float:
    return cleanFloat(field.in<float>(object));
  case Kind::String:
    return field.in<std::string>(object);
  case Kind::Vec2:
    return vecToJson<2>(&field.in<glm::vec2>(object).x);
  case Kind::Vec3:
    return vecToJson<3>(&field.in<glm::vec3>(object).x);
  case Kind::Vec4:
    return vecToJson<4>(&field.in<glm::vec4>(object).x);
  case Kind::Quat:
  {
    // [w, x, y, z]: the order glm's quat constructor takes.
    const glm::quat& q = field.in<glm::quat>(object);
    return json::array(
        {cleanFloat(q.w), cleanFloat(q.x), cleanFloat(q.y), cleanFloat(q.z)});
  }
  case Kind::Json:
    return field.in<json>(object);
  case Kind::StringList:
    return field.in<std::vector<std::string>>(object);
  case Kind::AssetRef:
    return field.in<AssetRefBase>(object).asset;
  case Kind::Mesh:
  {
    const MeshHandle mesh = field.in<MeshHandle>(object);
    const std::string_view name =
        meshes ? meshes->nameOf(mesh) : std::string_view();
    if (mesh.valid() && name.empty())
      LOG_ERROR("Saving field '" + field.name +
                "': the mesh has no name, so it can't be saved");
    return std::string(name);
  }
  }
  return nullptr;
}

// Assign `value` to the field; false if it has the wrong shape.
bool fieldFromJson(const reflect::FieldInfo& field,
                   void* object,
                   const json& value,
                   MeshLibrary* meshes)
{
  using Kind = reflect::FieldKind;
  switch (field.kind)
  {
  case Kind::Bool:
    if (!value.is_boolean())
      return false;
    field.in<bool>(object) = value.get<bool>();
    return true;
  case Kind::Int:
    if (!value.is_number_integer())
      return false;
    field.in<int32_t>(object) = value.get<int32_t>();
    return true;
  case Kind::UInt:
    if (!value.is_number_unsigned())
      return false;
    field.in<uint32_t>(object) = value.get<uint32_t>();
    return true;
  case Kind::Float:
    if (!value.is_number())
      return false;
    field.in<float>(object) = value.get<float>();
    return true;
  case Kind::String:
    if (!value.is_string())
      return false;
    field.in<std::string>(object) = value.get<std::string>();
    return true;
  case Kind::Vec2:
    return vecFromJson<2>(value, &field.in<glm::vec2>(object).x);
  case Kind::Vec3:
    return vecFromJson<3>(value, &field.in<glm::vec3>(object).x);
  case Kind::Vec4:
    return vecFromJson<4>(value, &field.in<glm::vec4>(object).x);
  case Kind::Quat:
  {
    float wxyz[4];
    if (!vecFromJson<4>(value, wxyz))
      return false;
    field.in<glm::quat>(object) = glm::quat(wxyz[0], wxyz[1], wxyz[2], wxyz[3]);
    return true;
  }
  case Kind::Json:
    field.in<json>(object) = value;
    return true;
  case Kind::AssetRef:
    if (!value.is_string())
      return false;
    field.in<AssetRefBase>(object).asset = value.get<std::string>();
    return true;
  case Kind::StringList:
  {
    if (!value.is_array())
      return false;
    std::vector<std::string> strings;
    for (const json& item : value)
    {
      if (!item.is_string())
        return false;
      strings.push_back(item.get<std::string>());
    }
    field.in<std::vector<std::string>>(object) = std::move(strings);
    return true;
  }
  case Kind::Mesh:
  {
    if (!value.is_string())
      return false;
    const std::string name = value.get<std::string>();
    MeshHandle mesh;
    if (!name.empty())
    {
      mesh = meshes ? meshes->resolve(name) : MeshHandle{};
      if (!mesh.valid() && meshes)
      {
        // Keep the name (a stand-in) so saving doesn't lose it; the mesh is
        // usually one a game module provides, before the module is built
        // (then it's expected, and not an error).
        if (!meshes->missingExpected())
          LOG_ERROR("Loading field '" + field.name + "': no mesh named '" +
                    name + "'");
        mesh = meshes->missing(name);
      }
    }
    field.in<MeshHandle>(object) = mesh;
    return true;
  }
  }
  return false;
}

// --- render settings
// -----------------------------------------------------------

// The scene's style: "3d", or a 2D game's plane ("flat" / "isometric") and
// how it maps to the screen.
json styleToJson(const std::optional<Space2D>& space)
{
  if (!space)
    return {{"kind", "3d"}};
  return {{"kind",
           space->projection == Space2D::Projection::Isometric ? "isometric"
                                                               : "flat"},
          {"pixelsPerUnit", cleanFloat(space->pixelsPerUnit)},
          {"tilePixels", toJson(space->tilePixels)},
          {"heightPixels", cleanFloat(space->heightPixels)},
          {"yDown", space->yDown},
          {"ySort", space->ySort}};
}

void styleFromJson(const json& in, RenderSystem& render)
{
  const std::string kind = valueOr<std::string>(in, "kind", "3d");
  if (kind != "flat" && kind != "isometric")
  {
    render.clearSpace2D();
    return;
  }
  Space2D space = render.space2D().value_or(Space2D{});
  space.projection = kind == "isometric" ? Space2D::Projection::Isometric
                                         : Space2D::Projection::Flat;
  space.pixelsPerUnit = valueOr(in, "pixelsPerUnit", space.pixelsPerUnit);
  space.tilePixels = vec2Or(in, "tilePixels", space.tilePixels);
  space.heightPixels = valueOr(in, "heightPixels", space.heightPixels);
  space.yDown = valueOr(in, "yDown", space.yDown);
  space.ySort = valueOr(in, "ySort", space.ySort);
  render.setSpace2D(space);
}

json renderToJson(const RenderSystem& render)
{
  const SceneLighting& lighting = render.lighting();
  const ShadowSettings& shadows = render.shadowSettings();
  const Camera& camera = render.camera();

  return {
      {"style", styleToJson(render.space2D())},
      {"lighting",
       {{"sunDirection", toJson(lighting.sun.direction)},
        {"sunColor", toJson(lighting.sun.color)},
        {"sunStrength", cleanFloat(lighting.sun.diffuseStrength)},
        {"ambient", cleanFloat(lighting.ambient)},
        {"skyColor", toJson(lighting.skyColor)}}},
      {"shadows",
       {{"enabled", shadows.enabled},
        {"distance", cleanFloat(shadows.distance)},
        {"depthBias", cleanFloat(shadows.depthBias)},
        {"softness", cleanFloat(shadows.softness)},
        {"pointLights", shadows.pointLights},
        {"pointResolution", shadows.pointResolution}}},
      {"wind",
       {{"direction", cleanFloat(render.windSettings().direction)},
        {"strength", cleanFloat(render.windSettings().strength)},
        {"gustiness", cleanFloat(render.windSettings().gustiness)}}},
      {"haze",
       {{"strength", cleanFloat(render.hazeSettings().strength)},
        {"distance", cleanFloat(render.hazeSettings().distance)}}},
      {"camera",
       {{"target", toJson(camera.target)},
        {"yaw", cleanFloat(camera.yaw)},
        {"pitch", cleanFloat(camera.pitch)},
        {"distance", cleanFloat(camera.distance)},
        {"orthographic", camera.projection == Camera::Projection::Orthographic},
        {"fieldOfView", cleanFloat(camera.fieldOfView)},
        {"orthoHeight", cleanFloat(camera.orthoHeight)}}},
  };
}

// Missing keys keep their current values, so older files load (as 3D: the
// only style there was).
void renderFromJson(const json& value, RenderSystem& render)
{
  if (const json& in = value.value("style", json::object()); in.is_object())
    styleFromJson(in, render);

  if (const json& in = value.value("lighting", json::object()); in.is_object())
  {
    SceneLighting lighting = render.lighting();
    lighting.sun.direction = vec3Or(in, "sunDirection", lighting.sun.direction);
    lighting.sun.color = vec3Or(in, "sunColor", lighting.sun.color);
    lighting.sun.diffuseStrength =
        valueOr(in, "sunStrength", lighting.sun.diffuseStrength);
    lighting.ambient = valueOr(in, "ambient", lighting.ambient);
    lighting.skyColor = vec3Or(in, "skyColor", lighting.skyColor);
    render.setLighting(lighting);
  }

  if (const json& in = value.value("shadows", json::object()); in.is_object())
  {
    ShadowSettings shadows = render.shadowSettings();
    shadows.enabled = valueOr(in, "enabled", shadows.enabled);
    shadows.distance = valueOr(in, "distance", shadows.distance);
    shadows.depthBias = valueOr(in, "depthBias", shadows.depthBias);
    shadows.softness = valueOr(in, "softness", shadows.softness);
    shadows.pointLights = valueOr(in, "pointLights", shadows.pointLights);
    shadows.pointResolution =
        valueOr(in, "pointResolution", shadows.pointResolution);
    render.setShadowSettings(shadows);
  }

  if (const json& in = value.value("wind", json::object()); in.is_object())
  {
    WindSettings wind = render.windSettings();
    wind.direction = valueOr(in, "direction", wind.direction);
    wind.strength = valueOr(in, "strength", wind.strength);
    wind.gustiness = valueOr(in, "gustiness", wind.gustiness);
    render.setWindSettings(wind);
  }

  if (const json& in = value.value("haze", json::object()); in.is_object())
  {
    HazeSettings haze = render.hazeSettings();
    haze.strength = valueOr(in, "strength", haze.strength);
    haze.distance = valueOr(in, "distance", haze.distance);
    render.setHazeSettings(haze);
  }

  if (const json& in = value.value("camera", json::object()); in.is_object())
  {
    Camera& camera = render.camera();
    camera.target = vec3Or(in, "target", camera.target);
    camera.yaw = valueOr(in, "yaw", camera.yaw);
    camera.pitch = valueOr(in, "pitch", camera.pitch);
    camera.distance = valueOr(in, "distance", camera.distance);
    camera.projection = valueOr(in, "orthographic", false)
                            ? Camera::Projection::Orthographic
                            : Camera::Projection::Perspective;
    camera.fieldOfView = valueOr(in, "fieldOfView", camera.fieldOfView);
    camera.orthoHeight = valueOr(in, "orthoHeight", camera.orthoHeight);
  }
}

// Indented JSON with short scalar arrays (vectors, colours) kept on one line,
// so scene files stay compact and diff cleanly under version control.
void writePretty(std::string& out, const json& value, int depth)
{
  const auto indent = [&](int level)
  { out.append(static_cast<std::size_t>(level) * 2, ' '); };

  if (value.is_object() && !value.empty())
  {
    out += "{\n";
    std::size_t i = 0;
    for (const auto& [key, member] : value.items())
    {
      indent(depth + 1);
      out += json(key).dump() + ": ";
      writePretty(out, member, depth + 1);
      out += ++i < value.size() ? ",\n" : "\n";
    }
    indent(depth);
    out += '}';
    return;
  }

  if (value.is_array() && !value.empty())
  {
    const bool scalars =
        std::all_of(value.begin(),
                    value.end(),
                    [](const json& v) { return v.is_primitive(); });
    if (scalars)
    {
      out += '[';
      for (std::size_t i = 0; i < value.size(); ++i)
        out += (i > 0 ? ", " : "") + value[i].dump();
      out += ']';
      return;
    }

    out += "[\n";
    for (std::size_t i = 0; i < value.size(); ++i)
    {
      indent(depth + 1);
      writePretty(out, value[i], depth + 1);
      out += i + 1 < value.size() ? ",\n" : "\n";
    }
    indent(depth);
    out += ']';
    return;
  }

  out += value.dump();
}

} // namespace

SceneSerializer::SceneSerializer(const reflect::TypeRegistry& types,
                                 MeshLibrary* meshes)
    : m_types(types), m_meshes(meshes)
{
}

json SceneSerializer::saveObject(const reflect::TypeInfo& type,
                                 void* object) const
{
  json fields = json::object();
  for (const reflect::FieldInfo& field : type.fields)
    fields[field.name] = fieldToJson(field, object, m_meshes);
  return fields;
}

void SceneSerializer::loadObject(const reflect::TypeInfo& type,
                                 void* object,
                                 const json& fields) const
{
  if (!fields.is_object())
    return;
  for (const reflect::FieldInfo& field : type.fields)
  {
    if (!fields.contains(field.name))
      continue; // new field since the save: keep its default
    if (!fieldFromJson(field, object, fields[field.name], m_meshes))
      LOG_ERROR("Loading " + type.name + "." + field.name +
                ": the saved value has the wrong type; kept the default");
  }
}

json SceneSerializer::saveEntity(Scene& scene, const Entity& entity) const
{
  return saveSubtree(entity, hierarchy::EntityTree(scene.entities()));
}

json SceneSerializer::saveComponents(const Entity& entity) const
{
  json components = json::object();
  for (const reflect::TypeInfo& type : m_types.types())
  {
    if (type.kind != reflect::TypeKind::Component)
      continue;
    void* object = type.component.get(entity);
    if (!object)
      continue;

    components[type.name] = saveObject(type, object);
  }

  // Components that didn't load (type not registered) go back out as read.
  if (entity.hasComponent<UnresolvedComponents>())
    for (const auto& [name, fields] :
         entity.getComponent<UnresolvedComponents>().components.items())
      if (!components.contains(name))
        components[name] = fields;
  return components;
}

json SceneSerializer::saveSubtree(const Entity& entity,
                                  const hierarchy::EntityTree& tree,
                                  bool ids) const
{
  // An instance whose prefab didn't load goes back out as it was read.
  if (entity.hasComponent<MissingPrefabComponent>())
    return entity.getComponent<MissingPrefabComponent>().entry;

  json entry;
  if (entity.hasComponent<PrefabInstanceComponent>())
    entry = instanceEntry(entity, tree); // the prefab, and how this one differs
  else
  {
    entry = saveComponents(entity);
    json children = json::array();
    for (const Entity& child : tree.children(entity))
      children.push_back(saveSubtree(child, tree, ids));
    if (!children.empty())
      entry[kChildrenKey] = std::move(children);
  }
  if (ids && entity.hasComponent<PrefabLinkComponent>())
    entry["$id"] = entity.getComponent<PrefabLinkComponent>().id;
  return entry;
}

Entity SceneSerializer::loadEntity(const json& entry,
                                   Scene& scene,
                                   const Entity& parent) const
{
  return loadEntity(entry, scene, parent, nullptr);
}

Entity SceneSerializer::loadEntity(const json& entry,
                                   Scene& scene,
                                   const Entity& parent,
                                   std::set<std::string>* unresolvedTypes) const
{
  if (entry.is_object() && entry.contains("$prefab"))
    return loadInstance(entry, scene, parent, unresolvedTypes);

  Entity entity = scene.createEntity();
  if (parent.isValid())
    hierarchy::setParent(entity, parent);
  if (!entry.is_object())
    return entity;
  loadComponents(entry, entity, unresolvedTypes);

  // After the parent's components, so children are created (and get ids)
  // after it, in saved order.
  if (const auto children = entry.find(kChildrenKey);
      children != entry.end() && children->is_array())
    for (const json& child : *children)
      if (child.is_object())
        loadEntity(child, scene, entity, unresolvedTypes);
  return entity;
}

void SceneSerializer::loadComponents(
    const json& entry,
    Entity& entity,
    std::set<std::string>* unresolvedTypes) const
{
  json unresolved = json::object();
  for (const auto& [typeName, fields] : entry.items())
  {
    if (typeName == kChildrenKey || typeName.starts_with('$'))
      continue;
    const reflect::TypeInfo* type = m_types.find(typeName);
    if (!type || type->kind != reflect::TypeKind::Component)
    {
      // Keep it rather than lose it: its module may just not be loaded.
      unresolved[typeName] = fields;
      continue;
    }

    type->component.add(entity);
    loadObject(*type, type->component.get(entity), fields);
  }

  if (!unresolved.empty())
  {
    if (unresolvedTypes)
      for (const auto& [typeName, fields] : unresolved.items())
        unresolvedTypes->insert(typeName);
    entity.addComponent<UnresolvedComponents>(
        UnresolvedComponents{std::move(unresolved)});
  }
}

json SceneSerializer::save(Scene& scene,
                           const std::vector<std::string>& gameSystems) const
{
  const hierarchy::EntityTree tree(scene.entities());
  json entities = json::array();
  for (const Entity& root : tree.roots())
    entities.push_back(saveSubtree(root, tree));

  json out = {{"formatVersion", kFormatVersion},
              {"systems", gameSystems},
              {"entities", std::move(entities)}};
  if (const RenderSystem* render = scene.tryGetSystem<RenderSystem>())
    out["render"] = renderToJson(*render);
  return out;
}

bool SceneSerializer::load(const json& in,
                           Scene& scene,
                           std::vector<std::string>* gameSystems) const
{
  if (!in.is_object() || !in.contains("entities") || !in["entities"].is_array())
  {
    LOG_ERROR("Scene data has no entity list");
    return false;
  }
  if (valueOr(in, "formatVersion", 0) > kFormatVersion)
  {
    LOG_ERROR("Scene was saved by a newer engine (format " +
              std::to_string(valueOr(in, "formatVersion", 0)) + ")");
    return false;
  }

  std::set<std::string> unresolvedTypes;
  for (const json& entry : in["entities"])
    if (entry.is_object())
      loadEntity(entry, scene, {}, &unresolvedTypes);

  if (!unresolvedTypes.empty())
  {
    std::string names;
    for (const std::string& name : unresolvedTypes)
      names += (names.empty() ? "" : ", ") + name;
    LOG_INFO("Scene uses components that aren't loaded (" + names +
             "); their data is kept and loads once they're built");
  }

  if (in.contains("render"))
    if (RenderSystem* render = scene.tryGetSystem<RenderSystem>())
      renderFromJson(in["render"], *render);

  if (gameSystems)
  {
    gameSystems->clear();
    for (const json& name : in.value("systems", json::array()))
      if (name.is_string())
        gameSystems->push_back(name.get<std::string>());
  }
  return true;
}

bool SceneSerializer::saveFile(
    Scene& scene,
    const std::filesystem::path& path,
    const std::vector<std::string>& gameSystems) const
{
  return writeJsonFile(save(scene, gameSystems), path);
}

bool SceneSerializer::loadFile(const std::filesystem::path& path,
                               Scene& scene,
                               std::vector<std::string>* gameSystems) const
{
  const std::optional<json> data = readJsonFile(path);
  return data && load(*data, scene, gameSystems);
}

bool SceneSerializer::writeJsonFile(const json& data,
                                    const std::filesystem::path& path)
{
  std::string text;
  writePretty(text, data, 0);
  text += '\n';
  // Never half a scene on disk, whatever happens while it's written.
  return writeFileAtomically(path, text);
}

std::optional<json>
SceneSerializer::readJsonFile(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
  {
    LOG_ERROR("Can't open " + path.string());
    return std::nullopt;
  }

  json data = json::parse(in, nullptr, false);
  if (data.is_discarded())
  {
    LOG_ERROR("Not valid JSON: " + path.string());
    return std::nullopt;
  }
  upgradeComponentNames(data);
  return data;
}

} // namespace sfs
