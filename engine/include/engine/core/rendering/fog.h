#pragma once

#include "glm/glm/ext/vector_float3.hpp"
#include "nlohmann/json.hpp"

namespace sfs
{

/**
 * Height fog: air that's thicker low down, as mist lies in a valley of a
 * morning. Its density falls off exponentially with height above
 * `baseHeight`, and what's seen through it fades toward the fog's colour: the
 * sky toward where you look (under a physical sky; under a flat one the sky
 * colour, as haze), brighter toward the sun, which the fog's droplets scatter
 * forward (a Mie glow).
 *
 * While it's on it takes haze's place (HazeSettings): the scene's haze isn't
 * drawn. Off (the default), haze is as it always was.
 *
 * @param enabled       whether there's fog at all
 * @param density       how thick it is at `baseHeight`: how much of the light
 *                      a metre of it takes (0.02: things 50 m away are about
 *                      two-thirds hidden)
 * @param heightFalloff how quickly it thins going up, per metre (0: as thick
 *                      at every height; 0.2: a fifth as thick 8 m up)
 * @param baseHeight    the height (world Z) it's `density` thick at
 * @param startDistance how far from the camera it starts (nothing nearer is
 *                      fogged)
 * @param maxOpacity    the most it hides anything, however far (1: all)
 * @param sunGlow       how much it glows toward the sun (0: none; 1 strong)
 */
struct FogSettings
{
  bool enabled = false;
  float density = 0.02f;
  float heightFalloff = 0.2f;
  float baseHeight = 0.0f;
  float startDistance = 0.0f;
  float maxOpacity = 1.0f;
  float sunGlow = 0.5f;

  nlohmann::json toJson() const;
  /** Missing keys keep `defaults`' values. */
  static FogSettings fromJson(const nlohmann::json& json,
                              const FogSettings& defaults);
  static FogSettings fromJson(const nlohmann::json& json);

  bool operator==(const FogSettings&) const = default;
};

/** How forward fog scatters the sun's light (Henyey-Greenstein's g). */
inline constexpr float kFogSunGlowSharpness = 0.75f;

/**
 * How much of a point fog hides, seen from `camera`: 0 (none) to the fog's
 * maxOpacity. The fog's density integrated along the line between them
 * (past startDistance), in closed form; the lit shader works it out the same
 * way (meshLit.wgsl's fogAt).
 */
float fogOpacity(const FogSettings& fog,
                 const glm::vec3& camera,
                 const glm::vec3& point);

/**
 * How much light the fog scatters toward the eye from a direction
 * `cosAngle` (the cosine) away from the light's: Henyey-Greenstein's phase
 * function, per steradian (integrates to 1 over the sphere).
 */
float fogPhase(float cosAngle, float sharpness = kFogSunGlowSharpness);

} // namespace sfs
