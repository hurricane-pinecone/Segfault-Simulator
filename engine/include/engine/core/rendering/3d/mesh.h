#pragma once

#include "engine/core/rendering/3d/aabb.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sfs
{

namespace materialGraph
{
class MaterialShader;
}

/**
 * One vertex of a 3D mesh. Colour is per-vertex albedo (linear RGB), so
 * untextured low-poly models carry their palette in the mesh itself; a
 * section's texture (sampled at `uv`) and an instance tint multiply it at
 * draw time.
 */
struct MeshVertex
{
  glm::vec3 position{0.0f};
  glm::vec3 normal{0.0f, 0.0f, 1.0f};
  glm::vec3 color{1.0f};
  glm::vec2 uv{0.0f}; // texture coordinates; (0, 0) is the image's top left
};

/**
 * A decoded image a mesh is textured with: 8-bit RGBA rows, top row first.
 * Colour images are sRGB (as image files store them); data images (metal and
 * roughness) are `linear`. Shared between the meshes that use it, so a
 * renderer can upload it once.
 */
struct TextureData
{
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba; // width * height * 4 bytes
  bool nearest = false;      // sample without smoothing (pixel art)
  bool linear = false;       // data, not colour: no sRGB conversion

  bool empty() const { return width == 0 || height == 0; }
};

/** How a material's alpha is used (glTF's alphaMode). */
enum class AlphaMode : uint8_t
{
  Opaque, // alpha ignored
  Mask,   // cut out where alpha is below alphaCutoff (leaves, fences)
  Blend,  // see-through by alpha (glass, water); drawn after opaque things
};

/**
 * How a surface looks, glTF's metallic-roughness model: the albedo is the
 * vertex colour x baseColor x baseTexture, with alpha from baseColor's x the
 * texture's. A null texture counts as white.
 *
 * Or, with a `shader` (a compiled material graph), whatever the graph works
 * out from its parameters and textures; then only alphaMode is read from the
 * rest.
 */
struct MeshMaterial
{
  glm::vec4 baseColor{1.0f}; // linear RGB, and alpha
  std::shared_ptr<const TextureData> baseTexture;
  // Metalness in blue and roughness in green (glTF's layout), multiplying the
  // factors below.
  std::shared_ptr<const TextureData> metallicRoughnessTexture;
  // A normal map (tangent space, OpenGL style: green up), read as data, and
  // how deep its bumps are (glTF's normalTexture.scale). Null: flat.
  std::shared_ptr<const TextureData> normalTexture;
  float normalScale = 1.0f;
  // Texture coordinates are uv x tiling + offset: tiling 4 repeats the
  // textures 4 times across what they covered once.
  glm::vec2 tiling{1.0f};
  glm::vec2 offset{0.0f};
  float metallic = 0.0f;  // 0: paint, wood, plastic; 1: bare metal
  float roughness = 1.0f; // 0: mirror-smooth; 1: matte
  AlphaMode alphaMode = AlphaMode::Opaque;
  float alphaCutoff = 0.5f; // for AlphaMode::Mask

  // A material graph's shader (see materialGraphCompiler.h), and the values
  // and textures it reads, in its order (a null texture reads white). Null:
  // the standard surface above.
  std::shared_ptr<const materialGraph::MaterialShader> shader;
  std::vector<glm::vec4> shaderParameters;
  std::vector<std::shared_ptr<const TextureData>> shaderTextures;

  bool operator==(const MeshMaterial&) const = default;
};

/**
 * A run of a mesh's indices drawn with one material: the mesh's own, unless an
 * object gives this section (its material slot) another.
 */
struct MeshSection
{
  uint32_t firstIndex = 0;
  uint32_t indexCount = 0;
  MeshMaterial material;
  std::string name; // the material's name in the model file, for people
};

/**
 * CPU-side mesh: an indexed triangle list (counter-clockwise front faces,
 * Z up). Plain data, so it can be built, combined and tested without a GPU;
 * a renderer uploads it once and hands back a MeshHandle.
 *
 * `sections` give runs of indices their own material (one material slot
 * each). With none, the whole mesh is one section with the default (matte,
 * white, untextured) material; with some, they cover every index.
 */
struct MeshData
{
  std::vector<MeshVertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<MeshSection> sections;

  bool empty() const { return indices.empty(); }

  /** Bounds of every vertex position. */
  Aabb bounds() const;
};

/**
 * Opaque reference to a mesh a renderer has uploaded. Value 0 is the null
 * handle; renderers never issue it.
 */
struct MeshHandle
{
  uint32_t id = 0;

  bool valid() const { return id != 0; }

  bool operator==(const MeshHandle&) const = default;
};

/**
 * Opaque reference to a material a renderer has uploaded on its own, for
 * objects to use in place of a section's own. Value 0 is the null handle.
 */
struct MaterialHandle
{
  uint32_t id = 0;

  bool valid() const { return id != 0; }

  bool operator==(const MaterialHandle&) const = default;
};

} // namespace sfs
