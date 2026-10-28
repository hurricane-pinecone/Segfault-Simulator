#pragma once

#include "engine/core/rendering/3d/mesh.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include "nlohmann/json.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace sfs
{

/**
 * A material as a .mat asset file stores it: MeshMaterial with its textures by
 * name ("asset:<id>" of a texture asset; empty for none), so it can be saved
 * and edited. Loading it resolves the names into a MeshMaterial.
 *
 * The file is JSON:
 *
 *   {
 *     "formatVersion": 1,
 *     "baseColor": [1, 1, 1, 1],
 *     "baseTexture": "asset:3f55f38a...",
 *     "metallicRoughnessTexture": "",
 *     "tiling": [1, 1], "offset": [0, 0],
 *     "metallic": 0, "roughness": 1,
 *     "alphaMode": "opaque", "alphaCutoff": 0.5
 *   }
 */
struct MaterialAsset
{
  static constexpr int kFormatVersion = 1;

  glm::vec4 baseColor{1.0f}; // linear RGB, and alpha
  std::string baseTexture;
  std::string metallicRoughnessTexture;
  glm::vec2 tiling{1.0f}; // how many times the textures repeat
  glm::vec2 offset{0.0f}; // shifts them, in texture widths
  float metallic = 0.0f;
  float roughness = 1.0f;
  AlphaMode alphaMode = AlphaMode::Opaque;
  float alphaCutoff = 0.5f;

  nlohmann::json toJson() const;

  /** Missing fields keep their defaults; nothing if `json` isn't an object. */
  static std::optional<MaterialAsset> fromJson(const nlohmann::json& json);

  bool operator==(const MaterialAsset&) const = default;
};

/** "opaque", "mask" or "blend". */
const char* alphaModeName(AlphaMode mode);

/** The mode a name from alphaModeName() means; nothing for any other name. */
std::optional<AlphaMode> alphaModeFromName(std::string_view name);

} // namespace sfs
