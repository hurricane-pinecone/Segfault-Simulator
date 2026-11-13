#include "engine/runtime/assetStore/modelImporter.h"

#include "engine/core/logger/logger.h"
#include "engine/core/util/hash.h"
#include "engine/runtime/assetStore/textureImporter.h"
#include "engine/runtime/serialization/prefabComponents.h"
#include "glm/glm/ext/matrix_float3x3.hpp"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/matrix_transform.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/gtc/type_ptr.hpp"
#include "glm/glm/matrix.hpp"
#include "nlohmann/json.hpp"

#ifdef SFS_HAS_MODEL_IMPORT
  #include <cgltf.h>
#endif

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>

namespace sfs
{

namespace
{
constexpr std::string_view kPrefix = "asset:";
} // namespace

std::string assetMeshName(const AssetId& model, std::string_view part)
{
  std::string name = std::string(kPrefix) + model.toString();
  if (!part.empty())
    name += ":" + std::string(part);
  return name;
}

std::optional<AssetMeshName> parseAssetMeshName(std::string_view name)
{
  if (!name.starts_with(kPrefix))
    return std::nullopt;
  name.remove_prefix(kPrefix.size());
  const std::optional<AssetId> id = AssetId::parse(name.substr(0, 32));
  if (!id)
    return std::nullopt;
  name.remove_prefix(std::min<std::size_t>(32, name.size()));
  if (!name.empty() && name.front() != ':')
    return std::nullopt;
  return AssetMeshName{*id, std::string(name.empty() ? name : name.substr(1))};
}

bool isColliderNodeName(std::string_view name)
{
  // Blender's ".001" on a copy.
  if (const std::size_t dot = name.rfind('.');
      dot != std::string_view::npos && dot + 1 < name.size() &&
      std::all_of(name.begin() + dot + 1,
                  name.end(),
                  [](char c) { return c >= '0' && c <= '9'; }))
    name = name.substr(0, dot);
  constexpr std::string_view kSuffix = "-collider";
  return name.size() > kSuffix.size() &&
         std::equal(kSuffix.begin(),
                    kSuffix.end(),
                    name.end() - kSuffix.size(),
                    [](char a, char b) {
                      return a == std::tolower(static_cast<unsigned char>(b));
                    });
}

namespace
{
// The id a name path's FNV-1a state gives: mixed, its low 31 bits kept.
uint32_t nodeIdOf(uint64_t pathHash)
{
  return kModelNodeIdBit |
         static_cast<uint32_t>(mix64(pathHash) & ~kModelNodeIdBit);
}
} // namespace

uint32_t stableNodeId(std::string_view namePath)
{
  return nodeIdOf(fnv1a64(namePath));
}

ModelImportSettings
ModelImportSettings::fromJson(const nlohmann::json& settings)
{
  ModelImportSettings result;
  if (settings.is_object() && settings.contains("scale") &&
      settings["scale"].is_number())
    result.scale = settings["scale"].get<float>();
  return result;
}

#ifdef SFS_HAS_MODEL_IMPORT

namespace
{
// glTF's Y-up axes to the engine's Z-up ones: +Y (up) becomes +Z and +Z
// (towards the viewer) becomes -Y. The inverse of Blender's glTF export, so a
// model comes back the way Blender showed it.
const glm::mat4 kYUpToZUp{1,
                          0,
                          0,
                          0, // column 0: x stays x
                          0,
                          0,
                          1,
                          0, // column 1: y becomes z
                          0,
                          -1,
                          0,
                          0, // column 2: z becomes -y
                          0,
                          0,
                          0,
                          1};

using DataPtr = std::unique_ptr<cgltf_data, decltype(&cgltf_free)>;

// The model's textures, decoded once each so every mesh using one shares it.
struct Textures
{
  std::filesystem::path folder; // where an image file's relative path starts
  struct KeyHash
  {
    std::size_t
    operator()(const std::pair<const cgltf_texture*, bool>& key) const
    {
      return std::hash<const void*>()(key.first) ^
             static_cast<std::size_t>(key.second);
    }
  };
  std::unordered_map<std::pair<const cgltf_texture*, bool>,
                     std::shared_ptr<const TextureData>,
                     KeyHash>
      loaded;
};

// The bytes of an image file the model refers to: inside a .glb, in a data:
// URI, or a file beside the model.
std::shared_ptr<const TextureData>
readImage(const cgltf_image& image, const std::filesystem::path& folder)
{
  const std::string name = image.name && *image.name ? image.name
                           : image.uri               ? image.uri
                                                     : "(unnamed)";
  if (image.buffer_view)
    return decodeTexture(cgltf_buffer_view_data(image.buffer_view),
                         image.buffer_view->size,
                         name);
  if (!image.uri)
    return nullptr;

  const std::string_view uri = image.uri;
  if (uri.starts_with("data:"))
  {
    const std::size_t comma = uri.find(',');
    if (comma == std::string_view::npos ||
        uri.substr(0, comma).find(";base64") == std::string_view::npos)
      return nullptr;
    const std::string_view base64 = uri.substr(comma + 1);
    std::size_t size = base64.size() / 4 * 3;
    for (std::size_t i = base64.size(); i > 0 && base64[i - 1] == '='; --i)
      --size;
    cgltf_options options{};
    void* bytes = nullptr;
    if (cgltf_load_buffer_base64(&options, size, base64.data(), &bytes) !=
        cgltf_result_success)
      return nullptr;
    std::shared_ptr<const TextureData> texture =
        decodeTexture(bytes, size, "(embedded)");
    std::free(bytes);
    return texture;
  }

  std::string relative(uri); // %20 and friends, as file names have them
  relative.resize(cgltf_decode_uri(relative.data()));
  const std::filesystem::path file = folder / relative;
  std::ifstream in(file, std::ios::binary);
  const std::vector<char> bytes{
      std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  if (bytes.empty())
  {
    LOG_ERROR("Model import: can't read the image " + file.string());
    return nullptr;
  }
  return decodeTexture(bytes.data(), bytes.size(), file.string());
}

// `linear`: a data texture (metal/roughness), not a colour one.
std::shared_ptr<const TextureData>
loadTexture(const cgltf_texture* texture, bool linear, Textures& textures)
{
  if (!texture || !texture->image)
    return nullptr;
  const auto key = std::make_pair(texture, linear);
  if (const auto it = textures.loaded.find(key); it != textures.loaded.end())
    return it->second;

  std::shared_ptr<const TextureData> loaded =
      readImage(*texture->image, textures.folder);
  const bool nearest = texture->sampler && texture->sampler->mag_filter ==
                                               cgltf_filter_type_nearest;
  if (loaded && (nearest || linear))
  {
    auto adjusted = std::make_shared<TextureData>(*loaded);
    adjusted->nearest = nearest;
    adjusted->linear = linear;
    loaded = std::move(adjusted);
  }
  textures.loaded.emplace(key, loaded); // a failure too, so it's tried once
  return loaded;
}

// Settings from a material's extras, as a generator writes them: a tree's or
// a plant's, {"sfs": {"foliage": {"translucency": 0.6, "bend": 1, ...}}}
// (see FoliageSettings), and {"sfs": {"fadeNearCamera": 0.4}} (foliage fades
// within 0.4 m without it: where the camera would be inside it). Left as they
// are without.
void readExtras(const cgltf_extras& extras, MeshMaterial& material)
{
  FoliageSettings& foliage = material.foliage;
  if (!extras.data)
    return;
  const nlohmann::json json =
      nlohmann::json::parse(extras.data, nullptr, false);
  if (!json.is_object() || !json.contains("sfs") || !json["sfs"].is_object())
    return;
  const nlohmann::json& sfs = json["sfs"];
  if (const auto fade = sfs.find("fadeNearCamera");
      fade != sfs.end() && fade->is_number())
    material.fadeNearCamera = fade->get<float>();
  const auto it = sfs.find("foliage");
  if (it == sfs.end() || !it->is_object())
    return;
  foliage.enabled = true;
  if (!sfs.contains("fadeNearCamera"))
    material.fadeNearCamera = 0.4f;
  const auto number = [&](const char* key, float& out)
  {
    if (const auto value = it->find(key);
        value != it->end() && value->is_number())
      out = value->get<float>();
  };
  number("translucency", foliage.translucency);
  number("bend", foliage.bend);
  number("sway", foliage.sway);
  number("flutter", foliage.flutter);
}

// The primitive's material, with its textures (only those its UVs can map).
MeshMaterial
readMaterial(const cgltf_material* source, bool hasUvs, Textures& textures)
{
  MeshMaterial material;
  if (!source)
    return material;
  if (source->has_pbr_metallic_roughness)
  {
    const cgltf_pbr_metallic_roughness& pbr = source->pbr_metallic_roughness;
    material.baseColor = glm::make_vec4(pbr.base_color_factor);
    material.metallic = pbr.metallic_factor;
    material.roughness = pbr.roughness_factor;
    // KHR_texture_transform's scale and offset (not its rotation) on the base
    // texture: some exporters tile textures that way rather than in the UVs.
    if (pbr.base_color_texture.has_transform)
    {
      const cgltf_texture_transform& transform =
          pbr.base_color_texture.transform;
      material.tiling = {transform.scale[0], transform.scale[1]};
      material.offset = {transform.offset[0], transform.offset[1]};
    }
    if (hasUvs)
    {
      material.baseTexture =
          loadTexture(pbr.base_color_texture.texture, false, textures);
      material.metallicRoughnessTexture =
          loadTexture(pbr.metallic_roughness_texture.texture, true, textures);
    }
  }
  if (hasUvs && source->normal_texture.texture)
  {
    material.normalTexture =
        loadTexture(source->normal_texture.texture, true, textures);
    material.normalScale = source->normal_texture.scale;
  }
  if (source->alpha_mode == cgltf_alpha_mode_mask)
    material.alphaMode = AlphaMode::Mask;
  else if (source->alpha_mode == cgltf_alpha_mode_blend)
    material.alphaMode = AlphaMode::Blend;
  material.alphaCutoff = source->alpha_cutoff;
  material.doubleSided = source->double_sided;
  readExtras(source->extras, material);
  return material;
}

glm::vec4
readVec(const cgltf_accessor* accessor, cgltf_size index, glm::vec4 fallback)
{
  if (!accessor)
    return fallback;
  float values[4] = {fallback.x, fallback.y, fallback.z, fallback.w};
  const cgltf_size components = cgltf_num_components(accessor->type);
  cgltf_accessor_read_float(accessor, index, values, components);
  return {values[0],
          values[1],
          values[2],
          components >= 4 ? values[3] : fallback.w};
}

// A vertex's skin: its four joints and their weights, normalised (none, all
// 0, if the primitive has no skin attributes or no weight).
SkinWeights readSkinWeights(const cgltf_accessor* joints,
                            const cgltf_accessor* weights,
                            cgltf_size index)
{
  SkinWeights out;
  if (!joints || !weights || index >= joints->count || index >= weights->count)
    return out;
  cgltf_uint ids[4] = {0, 0, 0, 0};
  cgltf_accessor_read_uint(joints, index, ids, 4);
  float w[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  cgltf_accessor_read_float(weights, index, w, 4);
  float total = 0.0f;
  for (int i = 0; i < 4; ++i)
  {
    w[i] = std::max(w[i], 0.0f);
    total += w[i];
  }
  if (total <= 1e-6f)
    return out;
  for (int i = 0; i < 4; ++i)
  {
    out.joints[i] = static_cast<uint16_t>(std::min<cgltf_uint>(ids[i], 65535));
    out.weights[i] = w[i] / total;
  }
  return out;
}

// Append a triangle primitive to `out`, placed by `transform`, as a section
// with its material; with `skinned`, each vertex's skin weights too.
void appendPrimitive(const cgltf_primitive& primitive,
                     const glm::mat4& transform,
                     Textures& textures,
                     MeshData& out,
                     bool skinned)
{
  if (primitive.type != cgltf_primitive_type_triangles)
    return; // points, lines and strips: rare in exported models

  const cgltf_accessor* positions = nullptr;
  const cgltf_accessor* normals = nullptr;
  const cgltf_accessor* colors = nullptr;
  const cgltf_accessor* uvs = nullptr;
  const cgltf_accessor* joints = nullptr;
  const cgltf_accessor* weights = nullptr;

  const cgltf_texture_view* baseTexture =
      primitive.material && primitive.material->has_pbr_metallic_roughness
          ? &primitive.material->pbr_metallic_roughness.base_color_texture
          : nullptr;
  const int uvSet = baseTexture ? baseTexture->texcoord : 0;

  for (cgltf_size i = 0; i < primitive.attributes_count; ++i)
  {
    const cgltf_attribute& attribute = primitive.attributes[i];
    if (attribute.type == cgltf_attribute_type_position)
      positions = attribute.data;
    else if (attribute.type == cgltf_attribute_type_normal)
      normals = attribute.data;
    else if (attribute.type == cgltf_attribute_type_color &&
             attribute.index == 0)
      colors = attribute.data;
    else if (attribute.type == cgltf_attribute_type_texcoord &&
             attribute.index == uvSet)
      uvs = attribute.data;
    // A skin's first four influences (JOINTS_1 and on, rare, are left out).
    else if (attribute.type == cgltf_attribute_type_joints &&
             attribute.index == 0)
      joints = attribute.data;
    else if (attribute.type == cgltf_attribute_type_weights &&
             attribute.index == 0)
      weights = attribute.data;
  }
  if (!positions)
    return;

  const glm::mat3 normalMatrix =
      glm::transpose(glm::inverse(glm::mat3(transform)));
  const auto first = static_cast<uint32_t>(out.vertices.size());
  for (cgltf_size i = 0; i < positions->count; ++i)
  {
    MeshVertex vertex;
    vertex.position = glm::vec3(
        transform *
        glm::vec4(glm::vec3(readVec(positions, i, glm::vec4(0.0f))), 1.0f));
    if (normals)
      vertex.normal = glm::normalize(
          normalMatrix * glm::vec3(readVec(normals, i, glm::vec4(0, 0, 1, 0))));
    vertex.color = glm::vec3(readVec(colors, i, glm::vec4(1.0f)));
    vertex.uv = glm::vec2(readVec(uvs, i, glm::vec4(0.0f)));
    out.vertices.push_back(vertex);
    if (skinned)
      out.skinWeights.push_back(readSkinWeights(joints, weights, i));
  }

  // A mirroring transform turns triangles inside out; swap two corners back.
  const bool mirrored = glm::determinant(glm::mat3(transform)) < 0.0f;
  const cgltf_size count =
      primitive.indices ? primitive.indices->count : positions->count;
  const auto firstIndex = static_cast<uint32_t>(out.indices.size());
  bool outOfRange = false;
  for (cgltf_size i = 0; i + 2 < count; i += 3)
  {
    cgltf_size corner[3];
    for (int c = 0; c < 3; ++c)
      corner[c] = primitive.indices
                      ? cgltf_accessor_read_index(primitive.indices, i + c)
                      : i + c;
    // A corner past the vertices (a broken or hostile file) would read
    // outside them: the triangle is left out.
    if (corner[0] >= positions->count || corner[1] >= positions->count ||
        corner[2] >= positions->count)
    {
      outOfRange = true;
      continue;
    }
    if (mirrored)
      std::swap(corner[1], corner[2]);
    for (const cgltf_size index : corner)
      out.indices.push_back(first + static_cast<uint32_t>(index));
  }
  if (outOfRange)
    LOG_ERROR("Model import: a mesh's triangles name vertices it doesn't "
              "have; those triangles are left out");

  // One section per primitive; one after another with the same material merge.
  MeshMaterial material =
      readMaterial(primitive.material, uvs != nullptr, textures);
  const auto indexCount =
      static_cast<uint32_t>(out.indices.size()) - firstIndex;
  if (indexCount > 0)
  {
    if (!out.sections.empty() && out.sections.back().material == material &&
        out.sections.back().firstIndex + out.sections.back().indexCount ==
            firstIndex)
      out.sections.back().indexCount += indexCount;
    else
    {
      const cgltf_material* source = primitive.material;
      std::string name =
          source && source->name && *source->name ? source->name : "";
      out.sections.push_back(
          {firstIndex, indexCount, std::move(material), std::move(name)});
    }
  }

  if (!normals)
  {
    // None in the file: average each vertex's face normals.
    for (uint32_t v = first; v < out.vertices.size(); ++v)
      out.vertices[v].normal = glm::vec3(0.0f);
    const std::size_t from = firstIndex;
    for (std::size_t i = from; i + 2 < out.indices.size(); i += 3)
    {
      MeshVertex& a = out.vertices[out.indices[i]];
      MeshVertex& b = out.vertices[out.indices[i + 1]];
      MeshVertex& c = out.vertices[out.indices[i + 2]];
      const glm::vec3 face =
          glm::cross(b.position - a.position, c.position - a.position);
      a.normal += face;
      b.normal += face;
      c.normal += face;
    }
    for (uint32_t v = first; v < out.vertices.size(); ++v)
    {
      const float length = glm::length(out.vertices[v].normal);
      out.vertices[v].normal =
          length > 0.0f ? out.vertices[v].normal / length : glm::vec3(0, 0, 1);
    }
  }
}

void appendMesh(const cgltf_mesh& mesh,
                const glm::mat4& transform,
                Textures& textures,
                MeshData& out,
                bool skinned = false)
{
  for (cgltf_size i = 0; i < mesh.primitives_count; ++i)
    appendPrimitive(mesh.primitives[i], transform, textures, out, skinned);
}

void appendNode(const cgltf_node& node,
                const glm::mat4& placement,
                Textures& textures,
                MeshData& out)
{
  if (node.mesh && !isColliderNodeName(node.name ? node.name : ""))
  {
    float world[16];
    cgltf_node_transform_world(&node, world);
    appendMesh(*node.mesh, placement * glm::make_mat4(world), textures, out);
  }
  for (cgltf_size i = 0; i < node.children_count; ++i)
    appendNode(*node.children[i], placement, textures, out);
}
} // namespace

namespace
{
// Whether a mesh has triangles to draw (read from the file's description of
// its data, so without loading the data).
bool hasTriangles(const cgltf_mesh& mesh)
{
  for (cgltf_size i = 0; i < mesh.primitives_count; ++i)
  {
    const cgltf_primitive& primitive = mesh.primitives[i];
    if (primitive.type != cgltf_primitive_type_triangles)
      continue;
    for (cgltf_size a = 0; a < primitive.attributes_count; ++a)
      if (primitive.attributes[a].type == cgltf_attribute_type_position &&
          primitive.attributes[a].data &&
          primitive.attributes[a].data->count >= 3)
        return true;
  }
  return false;
}

bool isColliderNode(const cgltf_node& node)
{
  return isColliderNodeName(node.name ? node.name : "");
}

// Whether any node is a collider (isColliderNodeName()).
bool hasColliderNodes(const cgltf_data& data)
{
  for (cgltf_size i = 0; i < data.nodes_count; ++i)
    if (data.nodes[i].mesh && isColliderNode(data.nodes[i]))
      return true;
  return false;
}

// Each mesh's part name, when the model has more than one to pick from:
// unique (files may repeat names), only for meshes with triangles that a node
// other than a collider draws.
std::unordered_map<const cgltf_mesh*, std::string>
partNames(const cgltf_data& data)
{
  std::unordered_map<const cgltf_mesh*, std::string> partOf;
  // With a skin, even one mesh is a part: its joints are nodes to place. With
  // a collider, its node is an entity, so the rest are too.
  if (data.meshes_count <= 1 && data.skins_count == 0 &&
      !hasColliderNodes(data))
    return partOf;
  std::set<const cgltf_mesh*> drawn;
  for (cgltf_size i = 0; i < data.nodes_count; ++i)
    if (data.nodes[i].mesh && !isColliderNode(data.nodes[i]))
      drawn.insert(data.nodes[i].mesh);
  std::set<std::string> taken;
  for (cgltf_size i = 0; i < data.meshes_count; ++i)
  {
    const std::string base = data.meshes[i].name && *data.meshes[i].name
                                 ? data.meshes[i].name
                                 : "Mesh " + std::to_string(i + 1);
    std::string name = base;
    for (int n = 2; taken.contains(name); ++n)
      name = base + " " + std::to_string(n);
    taken.insert(name);
    if (hasTriangles(data.meshes[i]) && drawn.contains(&data.meshes[i]))
      partOf[&data.meshes[i]] = name;
  }
  return partOf;
}

// Whether a positions accessor carries a min and max to read its bounds from
// (as glTF asks of every file), in plain floats.
bool hasFloatBounds(const cgltf_accessor& positions)
{
  return positions.has_min && positions.has_max &&
         positions.component_type == cgltf_component_type_r_32f &&
         !positions.normalized;
}

// A mesh's bounds in its own space, placed (axes and scale), as its parts'
// vertices are: from its positions' min and max, so without its data (read
// from the data instead where a file leaves them out, if it's loaded).
Aabb meshBounds(const cgltf_mesh& mesh, const glm::mat4& placement)
{
  Aabb bounds;
  for (cgltf_size p = 0; p < mesh.primitives_count; ++p)
  {
    const cgltf_primitive& primitive = mesh.primitives[p];
    if (primitive.type != cgltf_primitive_type_triangles)
      continue;
    for (cgltf_size a = 0; a < primitive.attributes_count; ++a)
    {
      const cgltf_accessor* positions = primitive.attributes[a].data;
      if (primitive.attributes[a].type != cgltf_attribute_type_position ||
          !positions)
        continue;
      if (hasFloatBounds(*positions))
      {
        bounds.expand(glm::make_vec3(positions->min));
        bounds.expand(glm::make_vec3(positions->max));
      }
      else if (positions->buffer_view && positions->buffer_view->buffer->data)
        for (cgltf_size k = 0; k < positions->count; ++k)
        {
          float v[3] = {0.0f, 0.0f, 0.0f};
          cgltf_accessor_read_float(positions, k, v, 3);
          bounds.expand(glm::make_vec3(v));
        }
    }
  }
  return bounds.empty() ? bounds : bounds.transformed(placement);
}

// Whether a collider's bounds need the file's data (a positions accessor
// without a min and max): importModelNodes then loads it.
bool collidersNeedData(const cgltf_data& data)
{
  for (cgltf_size i = 0; i < data.nodes_count; ++i)
  {
    const cgltf_node& node = data.nodes[i];
    if (!node.mesh || !isColliderNode(node))
      continue;
    for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p)
      for (cgltf_size a = 0; a < node.mesh->primitives[p].attributes_count; ++a)
      {
        const cgltf_attribute& attribute =
            node.mesh->primitives[p].attributes[a];
        if (attribute.type == cgltf_attribute_type_position && attribute.data &&
            !hasFloatBounds(*attribute.data))
          return true;
      }
  }
  return false;
}

// Writes a node's name path segment (see stableNodeId()) to `out`, a piece
// at a time: its name with "/", "#" and "\\" escaped, then "#k" for the k-th
// sibling with the same name.
template <typename Out>
void writeSegment(std::string_view name, uint32_t repeat, const Out& out)
{
  std::size_t from = 0;
  for (std::size_t i = 0; i < name.size(); ++i)
    if (name[i] == '/' || name[i] == '#' || name[i] == '\\')
    {
      out(name.substr(from, i - from));
      out("\\");
      from = i;
    }
  out(name.substr(from));
  if (repeat > 0)
  {
    char digits[16];
    const auto end = std::to_chars(digits, digits + sizeof(digits), repeat).ptr;
    out("#");
    out(std::string_view(digits, static_cast<std::size_t>(end - digits)));
  }
}

// Gives each node its stable id from its name path (see stableNodeId()),
// hashing each path on from its parent's, so no path is spelled out unless
// it's asked for (`namePaths`) or named in a warning. `names` are the nodes'
// own decoded names (empty: unnamed); `source` names the file in warnings.
void assignNodeIds(std::vector<ModelNode>& nodes,
                   const std::vector<std::string_view>& names,
                   const std::string& source,
                   std::vector<std::string>* namePaths)
{
  const std::size_t count = nodes.size();
  // Each node's place among its same-named siblings, in the file's order:
  // sorted by parent and name, a run of equal ones is numbered 0, 1, 2...
  struct Sibling
  {
    int parent;
    std::string_view name;
    std::size_t node;
  };
  std::vector<Sibling> siblings(count);
  for (std::size_t i = 0; i < count; ++i)
    siblings[i] = {nodes[i].parent, names[i], i};
  std::sort(siblings.begin(),
            siblings.end(),
            [](const Sibling& a, const Sibling& b)
            {
              return std::tie(a.parent, a.name, a.node) <
                     std::tie(b.parent, b.name, b.node);
            });
  std::vector<uint32_t> repeat(count, 0);
  std::vector<std::string_view> repeated;
  for (std::size_t i = 1; i < count; ++i)
    if (siblings[i].parent == siblings[i - 1].parent &&
        siblings[i].name == siblings[i - 1].name)
    {
      repeat[siblings[i].node] = repeat[siblings[i - 1].node] + 1;
      repeated.push_back(siblings[i].name);
    }

  const auto pathOf = [&](std::size_t node)
  {
    std::vector<std::size_t> chain;
    for (int at = static_cast<int>(node); at >= 0; at = nodes[at].parent)
      chain.push_back(static_cast<std::size_t>(at));
    std::string path;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    {
      if (it != chain.rbegin())
        path += '/';
      writeSegment(names[*it],
                   repeat[*it],
                   [&](std::string_view bytes) { path += bytes; });
    }
    return path;
  };

  // Preorder: a parent's hash is always made before its children's.
  std::vector<uint64_t> pathHash(count);
  std::unordered_map<uint32_t, std::size_t> taken;
  taken.reserve(count);
  for (std::size_t i = 0; i < count; ++i)
  {
    const int parent = nodes[i].parent;
    uint64_t hash =
        parent < 0 ? kFnv1a64Offset
                   : fnv1a64("/", pathHash[static_cast<std::size_t>(parent)]);
    writeSegment(names[i],
                 repeat[i],
                 [&](std::string_view bytes) { hash = fnv1a64(bytes, hash); });
    pathHash[i] = hash;
    uint32_t id = nodeIdOf(hash);
    if (const auto clash = taken.find(id); clash != taken.end())
    {
      LOG_WARN("Model import: " + source + ": the nodes '" +
               pathOf(clash->second) + "' and '" + pathOf(i) +
               "' hash to the same id; the second takes the next free one");
      while (taken.contains(id))
        id = kModelNodeIdBit | ((id + 1) & ~kModelNodeIdBit);
    }
    taken.emplace(id, i);
    nodes[i].id = id;
  }

  if (!repeated.empty())
  {
    std::sort(repeated.begin(), repeated.end());
    repeated.erase(
        std::unique(repeated.begin(), repeated.end()), repeated.end());
    std::string list;
    for (const std::string_view name : repeated)
      list += (list.empty() ? "'" : ", '") +
              (name.empty() ? std::string("(unnamed)") : std::string(name)) +
              "'";
    LOG_WARN("Model import: " + source + ": sibling nodes share the names " +
             list +
             "; their ids follow their order, so give them unique names in "
             "the source file");
  }
  if (namePaths)
  {
    namePaths->clear();
    namePaths->reserve(count);
    for (std::size_t i = 0; i < count; ++i)
      namePaths->push_back(pathOf(i));
  }
}

// The node tree, for placing the parts as entities. Each node's transform is
// moved into the engine's space: with M the placement (axes and scale),
// M * L * M^-1, so their product down the tree is M * (file's) * M^-1 and a
// part (whose vertices are already M * v) lands where the file put it.
std::vector<ModelNode>
readNodes(const cgltf_data& data,
          const glm::mat4& placement,
          const std::unordered_map<const cgltf_mesh*, std::string>& partOf,
          const std::string& source,
          std::vector<std::string>* namePaths = nullptr)
{
  std::vector<ModelNode> nodes;
  std::vector<const char*> rawNames;
  const glm::mat4 toFile = glm::inverse(placement);
  const std::function<void(const cgltf_node&, int)> addNode =
      [&](const cgltf_node& node, int parent)
  {
    rawNames.push_back(node.name ? node.name : "");
    float local[16];
    cgltf_node_transform_local(&node, local);
    ModelNode out;
    out.name = node.name && *node.name
                   ? node.name
                   : "Node " + std::to_string(nodes.size() + 1);
    out.parent = parent;
    out.local = placement * glm::make_mat4(local) * toFile;
    if (node.mesh)
    {
      out.collider = isColliderNode(node);
      out.bounds = meshBounds(*node.mesh, placement);
      if (const auto it = partOf.find(node.mesh);
          it != partOf.end() && !out.collider)
        out.part = it->second;
    }
    const int index = static_cast<int>(nodes.size());
    nodes.push_back(std::move(out));
    for (cgltf_size i = 0; i < node.children_count; ++i)
      addNode(*node.children[i], index);
  };
  const cgltf_scene* scene =
      data.scene ? data.scene : (data.scenes_count ? &data.scenes[0] : nullptr);
  if (scene)
    for (cgltf_size i = 0; i < scene->nodes_count; ++i)
      addNode(*scene->nodes[i], -1);
  else
    for (cgltf_size i = 0; i < data.nodes_count; ++i)
      if (!data.nodes[i].parent)
        addNode(data.nodes[i], -1);
  // glTF names are JSON strings, which cgltf leaves as spelled: decoded, so
  // an id doesn't depend on how the exporter wrote a name ("\\u00e9" or "é").
  std::vector<std::string> decoded;
  decoded.reserve(static_cast<std::size_t>(
      std::count_if(rawNames.begin(),
                    rawNames.end(),
                    [](const char* name) { return std::strchr(name, '\\'); })));
  std::vector<std::string_view> names;
  names.reserve(rawNames.size());
  for (const char* name : rawNames)
  {
    if (!std::strchr(name, '\\'))
    {
      names.emplace_back(name);
      continue;
    }
    std::string& text = decoded.emplace_back(name);
    text.resize(cgltf_decode_string(text.data()));
    names.emplace_back(text);
  }
  assignNodeIds(nodes, names, source, namePaths);
  return nodes;
}
// Each node's name as readNodes() gives it (its own, or "Node <n>" in the
// order it places them), which is what its entity is called: how a skin's
// joints and an animation's channels find theirs.
std::unordered_map<const cgltf_node*, std::string>
nodeNames(const cgltf_data& data)
{
  std::unordered_map<const cgltf_node*, std::string> names;
  std::size_t placed = 0;
  const std::function<void(const cgltf_node&)> addNode =
      [&](const cgltf_node& node)
  {
    names[&node] = node.name && *node.name
                       ? node.name
                       : "Node " + std::to_string(placed + 1);
    ++placed;
    for (cgltf_size i = 0; i < node.children_count; ++i)
      addNode(*node.children[i]);
  };
  const cgltf_scene* scene =
      data.scene ? data.scene : (data.scenes_count ? &data.scenes[0] : nullptr);
  if (scene)
    for (cgltf_size i = 0; i < scene->nodes_count; ++i)
      addNode(*scene->nodes[i]);
  else
    for (cgltf_size i = 0; i < data.nodes_count; ++i)
      if (!data.nodes[i].parent)
        addNode(data.nodes[i]);
  // Nodes outside the scene keep their own names.
  for (cgltf_size i = 0; i < data.nodes_count; ++i)
    if (!names.contains(&data.nodes[i]))
      names[&data.nodes[i]] = data.nodes[i].name ? data.nodes[i].name : "";
  return names;
}

// The skin a mesh is drawn with: the first node drawing it with one's.
const cgltf_skin* skinOf(const cgltf_data& data, const cgltf_mesh& mesh)
{
  for (cgltf_size i = 0; i < data.nodes_count; ++i)
    if (data.nodes[i].mesh == &mesh && data.nodes[i].skin)
      return data.nodes[i].skin;
  return nullptr;
}

// A skin in the engine's space: its joints by name, and each inverse bind
// matrix moved as nodes are (M * IBM * M^-1, with M the placement), since
// the skinned mesh's vertices are already M * v.
std::shared_ptr<MeshSkin>
readSkin(const cgltf_skin& skin,
         const glm::mat4& placement,
         const std::unordered_map<const cgltf_node*, std::string>& names)
{
  auto out = std::make_shared<MeshSkin>();
  const glm::mat4 toFile = glm::inverse(placement);
  for (cgltf_size j = 0; j < skin.joints_count; ++j)
  {
    const auto name = names.find(skin.joints[j]);
    out->joints.push_back(name != names.end() ? name->second : "");
    float bind[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (skin.inverse_bind_matrices)
      cgltf_accessor_read_float(skin.inverse_bind_matrices, j, bind, 16);
    out->inverseBind.push_back(placement * glm::make_mat4(bind) * toFile);
  }
  return out;
}

// A model's animations in the engine's space: the placement's axis swap
// (x, y, z) -> (x, -z, y) and scale applied to translations, the swap to
// rotations' axes and scales' axes. Morph target weights aren't read.
std::shared_ptr<const AnimationSet>
readAnimations(const cgltf_data& data,
               float scale,
               const std::unordered_map<const cgltf_node*, std::string>& names)
{
  if (data.animations_count == 0)
    return nullptr;
  auto set = std::make_shared<AnimationSet>();
  for (cgltf_size a = 0; a < data.animations_count; ++a)
  {
    const cgltf_animation& animation = data.animations[a];
    AnimationClip clip;
    clip.name = animation.name && *animation.name
                    ? animation.name
                    : "Animation " + std::to_string(a + 1);
    for (cgltf_size c = 0; c < animation.channels_count; ++c)
    {
      const cgltf_animation_channel& source = animation.channels[c];
      if (!source.target_node || !source.sampler || !source.sampler->input ||
          !source.sampler->output)
        continue;
      AnimationChannel channel;
      switch (source.target_path)
      {
      case cgltf_animation_path_type_translation:
        channel.path = AnimationPath::Translation;
        break;
      case cgltf_animation_path_type_rotation:
        channel.path = AnimationPath::Rotation;
        break;
      case cgltf_animation_path_type_scale:
        channel.path = AnimationPath::Scale;
        break;
      default:
        continue; // morph target weights: not supported
      }
      switch (source.sampler->interpolation)
      {
      case cgltf_interpolation_type_step:
        channel.interpolation = AnimationInterpolation::Step;
        break;
      case cgltf_interpolation_type_cubic_spline:
        channel.interpolation = AnimationInterpolation::CubicSpline;
        break;
      default:
        channel.interpolation = AnimationInterpolation::Linear;
        break;
      }
      const auto name = names.find(source.target_node);
      channel.node = name != names.end() ? name->second : "";
      const cgltf_accessor& input = *source.sampler->input;
      const cgltf_accessor& output = *source.sampler->output;
      channel.times.resize(input.count);
      for (cgltf_size k = 0; k < input.count; ++k)
        cgltf_accessor_read_float(&input, k, &channel.times[k], 1);
      const std::size_t stride =
          channel.interpolation == AnimationInterpolation::CubicSpline ? 3 : 1;
      if (output.count < input.count * stride)
        continue; // not enough values for its keys
      channel.values.resize(input.count * stride);
      const cgltf_size components = cgltf_num_components(output.type);
      for (std::size_t k = 0; k < channel.values.size(); ++k)
      {
        float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        cgltf_accessor_read_float(
            &output, k, v, std::min<cgltf_size>(components, 4));
        switch (channel.path)
        {
        case AnimationPath::Translation:
          channel.values[k] = glm::vec4(v[0], -v[2], v[1], 0.0f) * scale;
          break;
        case AnimationPath::Rotation:
          channel.values[k] = glm::vec4(v[0], -v[2], v[1], v[3]);
          break;
        case AnimationPath::Scale:
          channel.values[k] = glm::vec4(v[0], v[2], v[1], 0.0f);
          break;
        }
      }
      if (!channel.times.empty())
        clip.duration = std::max(clip.duration, channel.times.back());
      clip.channels.push_back(std::move(channel));
    }
    set->clips.push_back(std::move(clip));
  }
  return set;
}

// Whether a parsed file holds together (accessors inside their buffers,
// indices inside their vertices, no node its own ancestor): reading one that
// doesn't would go out of bounds. Logged if not.
bool valid(cgltf_data& data, const std::string& path)
{
  const cgltf_result result = cgltf_validate(&data);
  if (result == cgltf_result_success)
    return true;
  LOG_ERROR("Model import: " + path + " is damaged or not valid glTF (" +
            std::to_string(static_cast<int>(result)) + ")");
  return false;
}
} // namespace

std::optional<std::vector<ModelNode>>
importModelNodes(const std::filesystem::path& file,
                 const ModelImportSettings& settings,
                 std::vector<std::string>* namePaths)
{
  const std::string path = file.string();
  cgltf_options options{};
  cgltf_data* raw = nullptr;
  if (cgltf_parse_file(&options, path.c_str(), &raw) != cgltf_result_success)
  {
    LOG_ERROR("Model import: can't read " + path +
              " (only glTF: .glb and .gltf)");
    return std::nullopt;
  }
  DataPtr data(raw, cgltf_free);
  if (!valid(*data, path))
    return std::nullopt;
  // Only a file whose colliders' bounds aren't written down needs its data.
  if (collidersNeedData(*data) &&
      cgltf_load_buffers(&options, data.get(), path.c_str()) !=
          cgltf_result_success)
  {
    LOG_ERROR("Model import: can't load the data " + path + " refers to");
    return std::nullopt;
  }
  const glm::mat4 placement =
      glm::scale(glm::mat4(1.0f), glm::vec3(settings.scale)) * kYUpToZUp;
  const auto partOf = partNames(*data);
  // As importModel gives them: a tree only with parts (or colliders) to place.
  return partOf.empty() && !hasColliderNodes(*data)
             ? std::vector<ModelNode>()
             : readNodes(*data, placement, partOf, path, namePaths);
}

std::optional<ModelData> importModel(const std::filesystem::path& file,
                                     const ModelImportSettings& settings)
{
  const std::string path = file.string();
  cgltf_options options{};
  cgltf_data* raw = nullptr;
  if (cgltf_parse_file(&options, path.c_str(), &raw) != cgltf_result_success)
  {
    LOG_ERROR("Model import: can't read " + path +
              " (only glTF: .glb and .gltf)");
    return std::nullopt;
  }
  DataPtr data(raw, cgltf_free);
  if (cgltf_load_buffers(&options, data.get(), path.c_str()) !=
      cgltf_result_success)
  {
    LOG_ERROR("Model import: can't load the data " + path + " refers to");
    return std::nullopt;
  }
  if (!valid(*data, path))
    return std::nullopt;

  const glm::mat4 placement =
      glm::scale(glm::mat4(1.0f), glm::vec3(settings.scale)) * kYUpToZUp;
  ModelData model;
  Textures textures{file.parent_path(), {}};

  // The whole model: the default scene's nodes as placed (or every root node,
  // in a file without scenes).
  const cgltf_scene* scene =
      data->scene ? data->scene
                  : (data->scenes_count ? &data->scenes[0] : nullptr);
  if (scene)
    for (cgltf_size i = 0; i < scene->nodes_count; ++i)
      appendNode(*scene->nodes[i], placement, textures, model.whole);
  else
    for (cgltf_size i = 0; i < data->nodes_count; ++i)
      if (!data->nodes[i].parent)
        appendNode(data->nodes[i], placement, textures, model.whole);

  // Each mesh on its own, when there's more than one to pick from.
  const std::unordered_map<const cgltf_mesh*, std::string> partOf =
      partNames(*data);
  const auto names = nodeNames(*data);
  for (cgltf_size i = 0; i < data->meshes_count; ++i)
    if (const auto name = partOf.find(&data->meshes[i]); name != partOf.end())
    {
      ModelPart part;
      part.name = name->second;
      const cgltf_skin* skin = skinOf(*data, data->meshes[i]);
      appendMesh(
          data->meshes[i], placement, textures, part.mesh, skin != nullptr);
      if (skin && skin->joints_count > 0)
      {
        std::shared_ptr<MeshSkin> meshSkin = readSkin(*skin, placement, names);
        // Joints the vertices name that the skin doesn't have move nothing.
        for (SkinWeights& weights : part.mesh.skinWeights)
          for (int j = 0; j < 4; ++j)
            if (weights.joints[j] >= meshSkin->joints.size())
            {
              weights.joints[j] = 0;
              weights.weights[j] = 0.0f;
            }
        computeJointBounds(
            *meshSkin, part.mesh.vertices, part.mesh.skinWeights);
        part.mesh.skin = std::move(meshSkin);
      }
      else
        part.mesh.skinWeights.clear();
      model.parts.push_back(std::move(part));
    }
  model.animations = readAnimations(*data, settings.scale, names);

  // The node tree, for placing the parts (and colliders) as entities.
  if (!model.parts.empty() || hasColliderNodes(*data))
    model.nodes = readNodes(*data, placement, partOf, path);

  if (model.whole.empty())
  {
    LOG_ERROR("Model import: " + path + " has no triangles");
    return std::nullopt;
  }
  return model;
}

#else

std::optional<ModelData> importModel(const std::filesystem::path& file,
                                     const ModelImportSettings&)
{
  LOG_ERROR("Model import isn't part of this build: " + file.string());
  return std::nullopt;
}

std::optional<std::vector<ModelNode>>
importModelNodes(const std::filesystem::path& file,
                 const ModelImportSettings&,
                 std::vector<std::string>*)
{
  LOG_ERROR("Model import isn't part of this build: " + file.string());
  return std::nullopt;
}

#endif

} // namespace sfs
