#pragma once

#include "engine/core/rendering/ambientOcclusion.h"
#include "engine/core/rendering/colorGrading.h"
#include "engine/core/rendering/contactShadows.h"
#include "engine/core/rendering/fog.h"
#include "engine/core/rendering/lighting.h"
#include "engine/core/rendering/sky.h"
#include "glm/glm/ext/vector_float3.hpp"
#include "nlohmann/json.hpp"

#include <map>
#include <optional>
#include <string>

namespace sfs
{

/**
 * A visual style: a whole look for a scene, kept in a `.style` asset so a
 * game can switch between looks (say "Cozy" and "Realistic") while it's being
 * developed, or ship several. A scene names the style it uses in its render
 * settings (RenderSystem::setVisualStyle); with none, the scene's own
 * settings are the look.
 *
 * While a style is active:
 * - what it sets replaces the scene's own: its colour grading and tone
 *   mapping always; bloom (the camera's), haze, the sky, each shadow
 *   setting, ambient occlusion, contact shadows and fog only when the style has
 * them (each optional: left out, the scene's stays);
 * - the scene's light is re-balanced, not replaced, so a day/night cycle
 *   that sets the lighting every frame keeps working: the sun's and the
 *   ambient light's shares and the sky colour are multiplied (intensity x
 *   tint);
 * - materials with a variant for its `materialVariant` tag draw as that
 *   variant (see MaterialAsset::variants).
 *
 * The file is JSON (sections and keys optional; missing ones keep their
 * defaults, which change nothing):
 *
 *   {
 *     "formatVersion": 1,
 *     "grading": { ...ColorGrading::toJson()... },
 *     "lighting": {
 *       "sunIntensity": 1, "sunTint": [1, 1, 1],
 *       "ambientIntensity": 1, "ambientTint": [1, 1, 1],
 *       "skyIntensity": 1, "skyTint": [1, 1, 1]
 *     },
 *     "bloom": {"enabled": true, "intensity": 0.6, "threshold": 1,
 *               "softness": 0.5, "radius": 1},
 *     "haze": {"strength": 0.2, "distance": 120},
 *     "shadows": {"softness": 2.5, "cascades": 4, "cascadeDistance": 200,
 *                 "cascadeSplit": 0.75},
 *     "sky": { ...SkySettings::toJson()... },
 *     "ambientOcclusion": { ...AmbientOcclusionSettings::toJson()... },
 *     "contactShadows": { ...ContactShadowSettings::toJson()... },
 *     "fog": { ...FogSettings::toJson()... },
 *     "materialVariant": "realistic",
 *     "materials": {"farm:ground-paint": "asset:<id>"}
 *   }
 */
struct VisualStyle
{
  static constexpr int kFormatVersion = 1;

  ColorGrading grading;

  // The scene's light, re-balanced (see LightBalance): each share x intensity
  // x tint.
  float sunIntensity = 1.0f;
  glm::vec3 sunTint{1.0f};
  float ambientIntensity = 1.0f;
  glm::vec3 ambientTint{1.0f};
  float skyIntensity = 1.0f; // the sky colour (the background, haze, what
                             // shiny things reflect)
  glm::vec3 skyTint{1.0f};

  // Set: replaces the scene's (bloom: the scene camera's).
  std::optional<BloomSettings> bloom;
  std::optional<HazeSettings> haze;
  // The sun's shadows: each key of the "shadows" section separately.
  std::optional<float> shadowSoftness;
  std::optional<uint32_t> shadowCascades;
  std::optional<float> shadowCascadeDistance;
  std::optional<float> shadowCascadeSplit;
  // The sky (e.g. a physical one, with its image-based lighting).
  std::optional<SkySettings> sky;
  // Ambient occlusion, contact shadows and height fog (which, on, takes
  // haze's place).
  std::optional<AmbientOcclusionSettings> ambientOcclusion;
  std::optional<ContactShadowSettings> contactShadows;
  std::optional<FogSettings> fog;

  // Materials with a variant under this tag draw as it ("": none do).
  std::string materialVariant;
  // Materials drawn as others while it's on, by name (name -> material):
  // over their variants, and for materials a game makes in code too (e.g. a
  // ground it paints as it plays). See MeshLibrary::setMaterialOverrides().
  std::map<std::string, std::string> materials;

  /** The sun's and ambient's multipliers. */
  LightBalance lightBalance() const
  {
    return {sunTint * sunIntensity, ambientTint * ambientIntensity};
  }
  glm::vec3 skyScale() const { return skyTint * skyIntensity; }

  /** The scene's shadow settings, with this style's in place of its own. */
  ShadowSettings shadows(const ShadowSettings& scene) const;

  nlohmann::json toJson() const;
  /** Missing keys keep their defaults; nothing if `json` isn't an object. */
  static std::optional<VisualStyle> fromJson(const nlohmann::json& json);

  bool operator==(const VisualStyle&) const = default;
};

/** Bloom's settings as a style or scene file holds them. */
nlohmann::json bloomToJson(const BloomSettings& bloom);
BloomSettings bloomFromJson(const nlohmann::json& json,
                            const BloomSettings& defaults = BloomSettings());

} // namespace sfs
