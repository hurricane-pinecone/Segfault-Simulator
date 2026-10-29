#include "engine/runtime/assetStore/modelImporter.h"

#include "engine/core/logger/logger.h"
#include "engine/runtime/assetStore/textureImporter.h"
#include "glm/glm/ext/matrix_float3x3.hpp"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/matrix_transform.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/gtc/type_ptr.hpp"
#include "glm/glm/matrix.hpp"

#ifdef SFS_HAS_MODEL_IMPORT
  #include <cgltf.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <set>
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

// Append a triangle primitive to `out`, placed by `transform`, as a section
// with its material.
void appendPrimitive(const cgltf_primitive& primitive,
                     const glm::mat4& transform,
                     Textures& textures,
                     MeshData& out)
{
  if (primitive.type != cgltf_primitive_type_triangles)
    return; // points, lines and strips: rare in exported models

  const cgltf_accessor* positions = nullptr;
  const cgltf_accessor* normals = nullptr;
  const cgltf_accessor* colors = nullptr;
  const cgltf_accessor* uvs = nullptr;

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
  }

  // A mirroring transform turns triangles inside out; swap two corners back.
  const bool mirrored = glm::determinant(glm::mat3(transform)) < 0.0f;
  const cgltf_size count =
      primitive.indices ? primitive.indices->count : positions->count;
  const auto firstIndex = static_cast<uint32_t>(out.indices.size());
  for (cgltf_size i = 0; i + 2 < count; i += 3)
  {
    uint32_t corner[3];
    for (int c = 0; c < 3; ++c)
      corner[c] = first + static_cast<uint32_t>(
                              primitive.indices ? cgltf_accessor_read_index(
                                                      primitive.indices, i + c)
                                                : i + c);
    if (mirrored)
      std::swap(corner[1], corner[2]);
    out.indices.insert(out.indices.end(), corner, corner + 3);
  }

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
    const std::size_t from = out.indices.size() - (count / 3) * 3;
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
                MeshData& out)
{
  for (cgltf_size i = 0; i < mesh.primitives_count; ++i)
    appendPrimitive(mesh.primitives[i], transform, textures, out);
}

void appendNode(const cgltf_node& node,
                const glm::mat4& placement,
                Textures& textures,
                MeshData& out)
{
  if (node.mesh)
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

// Each mesh's part name, when the model has more than one to pick from:
// unique (files may repeat names), only for meshes with triangles.
std::unordered_map<const cgltf_mesh*, std::string>
partNames(const cgltf_data& data)
{
  std::unordered_map<const cgltf_mesh*, std::string> partOf;
  if (data.meshes_count <= 1)
    return partOf;
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
    if (hasTriangles(data.meshes[i]))
      partOf[&data.meshes[i]] = name;
  }
  return partOf;
}

// The node tree, for placing the parts as entities. Each node's transform is
// moved into the engine's space: with M the placement (axes and scale),
// M * L * M^-1, so their product down the tree is M * (file's) * M^-1 and a
// part (whose vertices are already M * v) lands where the file put it.
std::vector<ModelNode>
readNodes(const cgltf_data& data,
          const glm::mat4& placement,
          const std::unordered_map<const cgltf_mesh*, std::string>& partOf)
{
  std::vector<ModelNode> nodes;
  const glm::mat4 toFile = glm::inverse(placement);
  const std::function<void(const cgltf_node&, int)> addNode =
      [&](const cgltf_node& node, int parent)
  {
    float local[16];
    cgltf_node_transform_local(&node, local);
    ModelNode out;
    out.name = node.name && *node.name
                   ? node.name
                   : "Node " + std::to_string(nodes.size() + 1);
    out.parent = parent;
    out.local = placement * glm::make_mat4(local) * toFile;
    if (node.mesh)
      if (const auto it = partOf.find(node.mesh); it != partOf.end())
        out.part = it->second;
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
  return nodes;
}
} // namespace

std::optional<std::vector<ModelNode>>
importModelNodes(const std::filesystem::path& file,
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
  const glm::mat4 placement =
      glm::scale(glm::mat4(1.0f), glm::vec3(settings.scale)) * kYUpToZUp;
  const auto partOf = partNames(*data);
  // As importModel gives them: a tree only with parts to place.
  return partOf.empty() ? std::vector<ModelNode>()
                        : readNodes(*data, placement, partOf);
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
  for (cgltf_size i = 0; i < data->meshes_count; ++i)
    if (const auto name = partOf.find(&data->meshes[i]); name != partOf.end())
    {
      ModelPart part;
      part.name = name->second;
      appendMesh(data->meshes[i], placement, textures, part.mesh);
      model.parts.push_back(std::move(part));
    }

  // The node tree, for placing the parts as entities.
  if (!model.parts.empty())
    model.nodes = readNodes(*data, placement, partOf);

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
importModelNodes(const std::filesystem::path& file, const ModelImportSettings&)
{
  LOG_ERROR("Model import isn't part of this build: " + file.string());
  return std::nullopt;
}

#endif

} // namespace sfs
