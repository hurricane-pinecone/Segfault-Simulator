#pragma once

#include "nlohmann/json.hpp"

#include <cstdint>

namespace sfs
{

/**
 * Screen-space ambient occlusion: the light from all around (the sky, the
 * ambient level) is kept out of creases, corners and the ground at the foot
 * of walls, where the rest of the scene blocks most of the sky. Worked out
 * from the depth and normals of what's drawn, at half the screen's
 * resolution, then blurred (along edges, not across them). It darkens only
 * that light and what shiny things reflect, never the sun's or point
 * lights'.
 *
 * Grass blades don't occlude (they'd darken whole fields); the ground under
 * them does, so blades near a wall are shaded by it as the ground is.
 *
 * @param enabled   whether to at all (off by default: costs nothing)
 * @param radius    how far around a point it looks for what blocks it,
 *                  world units (1: creases and the foot of walls; bigger
 *                  darkens more broadly)
 * @param intensity how dark it gets (0: none; 1 normal; up to 4)
 * @param samples   how many points around each pixel it looks at (4 to 32):
 *                  more is smoother and costs more
 */
struct AmbientOcclusionSettings
{
  bool enabled = false;
  float radius = 1.0f;
  float intensity = 1.0f;
  uint32_t samples = 12;

  nlohmann::json toJson() const;
  /** Missing keys keep `defaults`' values. */
  static AmbientOcclusionSettings
  fromJson(const nlohmann::json& json,
           const AmbientOcclusionSettings& defaults);
  static AmbientOcclusionSettings fromJson(const nlohmann::json& json);

  bool operator==(const AmbientOcclusionSettings&) const = default;
};

/** The fewest and most samples AmbientOcclusionSettings::samples may be. */
inline constexpr uint32_t kMinOcclusionSamples = 4;
inline constexpr uint32_t kMaxOcclusionSamples = 32;

} // namespace sfs
