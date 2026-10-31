#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include "glm/glm/ext/vector_int2.hpp"
#include "nlohmann/json.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sfs
{

struct TextureData;

/**
 * Most layers a terrain material blends: its first (everywhere nothing else
 * is painted) and one for each of the splat map's four channels over it.
 */
inline constexpr std::size_t kMaxTerrainLayers = 5;

/**
 * One of a terrain's layers: a ground (grass, dirt, a path, rock) as tiling
 * textures, laid over the world from above so a big field doesn't stretch
 * them, and broken up so it doesn't repeat (see TerrainMaterial).
 *
 * @param baseTexture      its colour (sRGB); the alpha is its height, which
 *                         layers blend by (higher shows through: stones
 *                         poking through the dirt between them). Opaque: flat
 * @param normalTexture    tangent space (OpenGL style), read as data; null:
 *                         flat
 * @param roughnessTexture read as data: green (glTF's channel) x `roughness`;
 *                         null: `roughness` all over
 * @param tint             its colour, x (linear)
 * @param tileSize         world units across one repeat of its textures
 * @param grass            how much of a grass field on the mesh grows over it
 *                         (1: as its maps say; 0: none, bare dirt)
 */
struct TerrainLayer
{
  std::string name; // for people ("Grass", "Dirt")
  std::shared_ptr<const TextureData> baseTexture;
  std::shared_ptr<const TextureData> normalTexture;
  std::shared_ptr<const TextureData> roughnessTexture;
  glm::vec3 tint{1.0f};
  float tileSize = 4.0f;
  float roughness = 1.0f;
  float normalScale = 1.0f;
  float grass = 1.0f;

  bool operator==(const TerrainLayer&) const = default;
};

/**
 * A terrain material: up to kMaxTerrainLayers ground layers blended by a
 * splat map (a weight map over the mesh's texture coordinates). Its first
 * layer is everywhere; the splat map's red, green, blue and alpha paint the
 * second to fifth over it, each from 0 (none) to 1 (only it). Where they add
 * up to more than 1 they share it; the first gets what's left.
 *
 * Each layer's textures are laid over the world's XY from above, `tileSize`
 * across, so ground of any size (a 200 m field) keeps its texel size. With
 * `antiTiling`, each is sampled three times on a hexagonal grid of randomly
 * shifted and turned copies, blended (Mikkelsen's hex-tiling), so it doesn't
 * repeat, and its brightness varies gently over tens of metres
 * (`variation`). Layers blend by height (`blendSharpness`): at 0 a smooth
 * cross-fade, toward 1 a crisp edge where the higher layer pokes through.
 *
 * Grass on the mesh (GrassComponent) grows where its layers' `grass` says,
 * x its own maps. A vertex colour isn't used.
 */
struct TerrainMaterial
{
  // RGBA weights of layers 2-5 (read as data, sampled smoothly, clamped at
  // the edges); null: the first layer only.
  std::shared_ptr<const TextureData> splat;
  // A colour over the mesh's UVs multiplying the layers (sRGB; null: none):
  // what a game paints on its ground (e.g. tilled fields), kept when the
  // terrain stands in for the game's material (MeshLibrary sets it).
  std::shared_ptr<const TextureData> tint;
  std::vector<TerrainLayer> layers;
  float blendSharpness = 0.5f;
  bool antiTiling = true;
  float variation = 0.15f;

  bool operator==(const TerrainMaterial&) const = default;
};

/**
 * Each layer's weight (the first to the fifth; summing to 1) for a splat map
 * texel (0 to 1 a channel), before blending by height.
 */
std::array<float, kMaxTerrainLayers> terrainWeights(const glm::vec4& splat);

/** A splat map's value at `uv` (bilinear, clamped at its edges), 0 to 1. */
glm::vec4 sampleSplat(const TextureData& splat, glm::vec2 uv);

/**
 * How much grass grows at `uv` on a terrain (its layers' `grass` by their
 * weight there): 1 with no splat map or layers.
 */
float terrainGrass(const TerrainMaterial& terrain, glm::vec2 uv);

/** Whether any of a terrain's layers keeps grass from growing. */
bool terrainLimitsGrass(const TerrainMaterial& terrain);

/**
 * Paint layer `layer` (0 the first, 1-4 the splat channels) into a splat map
 * over an ellipse in UV space (`radius` across in u and v): full `strength`
 * (0 to 1) over its inner part, easing to nothing at its edge (`softness` of
 * the radius). Painting a layer moves its weight toward 1 and the others'
 * toward 0 (the first: all of them toward 0), so they never sum past 1.
 *
 * @return the texels changed (min inclusive, max exclusive; empty if none)
 */
std::pair<glm::ivec2, glm::ivec2> paintSplat(TextureData& splat,
                                             glm::vec2 center,
                                             glm::vec2 radius,
                                             int layer,
                                             float strength,
                                             float softness = 0.5f);

/** A splat map `size` texels square, all the first layer (all 0). */
std::shared_ptr<TextureData> blankSplat(int size);

/** A terrain layer as a .mat file holds it: textures by name ("asset:<id>"). */
struct TerrainLayerAsset
{
  std::string name; // for people ("Grass", "Dirt")
  std::string baseTexture;
  std::string normalTexture;
  std::string roughnessTexture;
  glm::vec3 tint{1.0f};
  float tileSize = 4.0f;
  float roughness = 1.0f;
  float normalScale = 1.0f;
  float grass = 1.0f;

  bool operator==(const TerrainLayerAsset&) const = default;
};

/**
 * A terrain material as a .mat file holds it (MaterialAsset::terrain):
 *
 *   "terrain": {
 *     "splatMap": "asset:<id>",
 *     "blendSharpness": 0.5, "antiTiling": true, "variation": 0.15,
 *     "layers": [{"name": "Grass", "baseTexture": "asset:<id>",
 *                 "normalTexture": "", "roughnessTexture": "",
 *                 "tint": [1, 1, 1], "tileSize": 4, "roughness": 1,
 *                 "normalScale": 1, "grass": 1}, ...]
 *   }
 */
struct TerrainAsset
{
  std::string splatMap;
  std::vector<TerrainLayerAsset> layers;
  float blendSharpness = 0.5f;
  bool antiTiling = true;
  float variation = 0.15f;

  nlohmann::json toJson() const;
  /** Missing keys keep their defaults (at most kMaxTerrainLayers layers). */
  static TerrainAsset fromJson(const nlohmann::json& json);

  bool operator==(const TerrainAsset&) const = default;
};

} // namespace sfs
