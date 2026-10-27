#pragma once

#include "glm/glm/ext/vector_float3.hpp"

namespace sfs
{

/**
 * The directional sun. Same knobs as the isometric path's ambient lighting, so
 * a day/night controller drives either renderer the same way.
 *
 * @param direction       vector pointing toward the sun (need not be unit
 *                        length); Z is up, so Z <= 0 means the sun has set
 * @param color           linear RGB tint of sunlight
 * @param diffuseStrength how much a surface facing the sun brightens over
 *                        ambient (0..1)
 */
struct SunLight3D
{
  glm::vec3 direction{-0.45f, -0.35f, 0.82f};
  glm::vec3 color{1.0f, 0.96f, 0.88f};
  float diffuseStrength = 0.85f;
};

/**
 * Scene-wide lighting: the sun plus the ambient level and sky colour.
 *
 * @param ambient  base brightness everything receives (0 = black night,
 *                 1 = fully lit); also gates point lights, which fade in as
 *                 ambient drops, exactly like the isometric renderer
 * @param skyColor linear RGB background clear colour
 */
struct SceneLighting3D
{
  SunLight3D sun;
  float ambient = 0.55f;
  glm::vec3 skyColor{0.55f, 0.72f, 0.90f};
};

/**
 * One point light, resolved to world space for a frame.
 *
 * @param position  world position
 * @param color     linear RGB tint
 * @param intensity brightness multiplier (0 = off)
 * @param radius    reach in world units; contributes nothing beyond it
 */
struct PointLight3D
{
  glm::vec3 position{0.0f};
  glm::vec3 color{1.0f, 0.85f, 0.6f};
  float intensity = 1.0f;
  float radius = 8.0f;
};

/**
 * Sun shadow-map settings.
 *
 * @param enabled    whether the sun casts shadows at all
 * @param distance   radius (world units) around the camera target that
 *                   receives shadows; larger covers more ground at lower
 *                   detail
 * @param depthBias  depth offset that prevents self-shadowing "acne"
 * @param softness   filter radius in shadow-map texels (0 = hard edges)
 */
struct ShadowSettings3D
{
  bool enabled = true;
  float distance = 40.0f;
  float depthBias = 0.0015f;
  float softness = 1.5f;
};

} // namespace sfs
