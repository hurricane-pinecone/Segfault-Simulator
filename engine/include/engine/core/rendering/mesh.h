#pragma once

#include "engine/core/rendering/aabb.h"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sfs
{

namespace materialGraph
{
class MaterialShader;
}
struct TerrainMaterial;

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
  Opaque,   // alpha ignored
  Mask,     // cut out where alpha is below alphaCutoff (leaves, fences)
  Blend,    // see-through by alpha (glass, water); drawn after opaque things
  Additive, // its light added to what's behind (fire, sparks, glows); drawn
            // after opaque things, casting no shadow
};

/** Whether a mode draws see-through, after everything opaque. */
inline bool isSeeThrough(AlphaMode mode)
{
  return mode == AlphaMode::Blend || mode == AlphaMode::Additive;
}

/**
 * A tree's or a plant's parts (MeshMaterial::foliage): drawn with the foliage
 * surface, which reads the mesh's vertex colours as data rather than colour
 * (R how open the point is: it multiplies the albedo, darker deep in a crown;
 * G how much it sways with its branch, 0 on the trunk to 1 at the twigs' tips;
 * B how much it flutters, 0 on bark, up to 1 at a leaf card's far corners),
 * moves its vertices in the frame's wind (the whole plant bending by its height
 * above its instance's origin, its branches swaying, its leaves fluttering; in
 * every pass, so its shadow moves too), lets sunlight through from behind and
 * keeps its normals on back faces (leaf cards' normals bent out from the
 * crown's middle shade it as one soft volume). A masked one keeps its cut-outs
 * solid as its textures shrink into mipmaps, and fades by dithering where an
 * instance asks (see MeshLodComponent's cross-fades).
 *
 * @param enabled      draw with the foliage surface
 * @param translucency how much sunlight shows through it from behind (0 none,
 *                     1 a thin leaf's), x the metallic-roughness texture's red
 *                     (its thinness: glTF leaves that channel unused)
 * @param bend         how far the whole plant bends in the wind (1 a tree's)
 * @param sway         how far its branches sway (x the vertices' G)
 * @param flutter      how much its leaves flutter (x the vertices' B)
 */
struct FoliageSettings
{
  bool enabled = false;
  float translucency = 0.5f;
  float bend = 1.0f;
  float sway = 1.0f;
  float flutter = 1.0f;

  bool operator==(const FoliageSettings&) const = default;
};

/**
 * How a surface looks, glTF's metallic-roughness model: the albedo is the
 * vertex colour x baseColor x baseTexture, with alpha from baseColor's x the
 * texture's. A null texture counts as white.
 *
 * Or, with a `shader` (a compiled material graph), whatever the graph works
 * out from its parameters and textures; then only alphaMode is read from the
 * rest. Or, with a `terrain`, its layers (and nothing else).
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
  // Shows its colour as it is, whatever light falls on it (particles, glows,
  // vivid marks): drawn as emission.
  bool unlit = false;
  // Both its sides drawn (glTF's doubleSided: thin sheets, and models made
  // with their faces turned either way), a back face lit as seen from
  // behind. Otherwise only its front faces are.
  bool doubleSided = false;
  // A tree's or a plant's: the foliage surface (see FoliageSettings), in
  // place of the standard one. Ignored with a shader or a terrain.
  FoliageSettings foliage;
  // Something the camera passes through rather than bumping into (a tree, a
  // bush): within this many metres of the eye (where the camera would be
  // inside it) it fades away by dithering (all back by 1.5 times as far), and
  // round the line of sight to what the camera looks at if the frame asks
  // (FrameParams::lineOfSight; off by default), in the camera's view only
  // (its shadow stays). 0: never. The foliage surface's, and a masked
  // standard one's.
  float fadeNearCamera = 0.0f;

  // A material graph's shader (see materialGraphCompiler.h), and the values
  // and textures it reads, in its order (a null texture reads white). Null:
  // the standard surface above.
  std::shared_ptr<const materialGraph::MaterialShader> shader;
  std::vector<glm::vec4> shaderParameters;
  std::vector<std::shared_ptr<const TextureData>> shaderTextures;
  // A terrain's layers and splat map (see TerrainMaterial), drawn in place of
  // the standard surface (and of a shader). Null: none.
  std::shared_ptr<const TerrainMaterial> terrain;
  // What a game paints on its ground, for a terrain standing in for this
  // material (MeshLibrary::setMaterialOverrides(), a style's variant): its
  // layers' weights (TerrainMaterial::paint) and its tint (::tint; null: the
  // base texture). The standard surface ignores them. Both are uploaded
  // without mipmaps, so IRenderer::updateTexture() can change them in place.
  std::shared_ptr<const TextureData> terrainPaint;
  std::shared_ptr<const TextureData> terrainTint;

  // Materials (by name, e.g. "asset:<id>") to draw instead while a visual
  // style with the tag is active: (tag, material) pairs (see
  // MaterialAsset::variants, MeshLibrary::setMaterialVariant).
  std::vector<std::pair<std::string, std::string>> variants;

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
 * One vertex's skin (see MeshSkin): up to four joints, by their place in the
 * skin's joints, and how much each moves it (summing to 1; all 0: it stays
 * as it is).
 */
struct SkinWeights
{
  std::array<uint16_t, 4> joints{0, 0, 0, 0};
  glm::vec4 weights{0.0f};
};

/**
 * What a skinned mesh (a character's body) moves with: its joints, the nodes
 * of its model (entities, by name) that deform it, each with its inverse
 * bind matrix (from the mesh's own space to the joint's, as the mesh was
 * bound to it) and the box around the vertices it moves (in the mesh's own
 * space, for bounds that hold whatever the pose; see skinnedBounds()).
 */
struct MeshSkin
{
  std::vector<std::string> joints;
  std::vector<glm::mat4> inverseBind;
  std::vector<Aabb> jointBounds;
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
  // A skinned mesh's skeleton, and each vertex's weights (as many as
  // vertices); null and empty for a mesh that doesn't deform.
  std::shared_ptr<const MeshSkin> skin;
  std::vector<SkinWeights> skinWeights;

  bool empty() const { return indices.empty(); }
  bool skinned() const
  {
    return skin && !skin->joints.empty() &&
           skinWeights.size() == vertices.size();
  }

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
