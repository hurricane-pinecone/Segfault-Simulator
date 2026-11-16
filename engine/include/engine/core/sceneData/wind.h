#pragma once

#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

/**
 * The scene's wind, as Unity's directional WindZone or Unreal's
 * WindDirectionalSource: its physical properties only. Foliage materials,
 * grass and the ground's sheen sway in it, as hard as its speed makes them
 * (windSway()), gusts rolling across the land the way it blows; it carries
 * particles (ParticleEffectDesc::windInfluence), clouds and rain. A scene
 * without one has still air. Local gusts are WindZoneComponents, which add to
 * it within their reach. A game's weather usually writes it each frame; the
 * renderer and the particles read it as it is.
 */
struct [[sfs::scene_data]] Wind
{
  /** Which way it blows, in degrees from +X toward +Y. */
  [[sfs::range(0, 360)]] float direction = 30.0f;
  /** How fast the air moves, in metres a second, along `direction`. */
  [[sfs::range(0, 40)]] float speed = 4.0f;
  /** How much it comes and goes in gusts (0: steady). */
  [[sfs::range(0, 1)]] float gustiness = 0.5f;
};

/** How hard things sway in a breeze of kBreezeSpeed (see windSway()). */
inline constexpr float kSwayAtBreeze = 0.35f;
/** A moderate breeze, metres a second: what kSwayAtBreeze is for. */
inline constexpr float kBreezeSpeed = 4.0f;
/**
 * The least and the most things sway, as shares of a breeze's sway: a light
 * stir even in calm air, and no more than a gale's.
 */
inline constexpr float kLeastSway = 0.4f;
inline constexpr float kMostSway = 2.5f;

/**
 * The wind's velocity across the world's XY, in metres a second: `speed`
 * along `direction`. Zero for a speed that isn't a finite number.
 *
 * @param wind the wind
 * @return its velocity (m/s)
 */
glm::vec2 windVelocity(const Wind& wind);

/**
 * How hard things sway in the wind (0: still; about 1: a gale): what foliage
 * materials, grass and the ground's sheen move by. In proportion to its speed,
 * kSwayAtBreeze at kBreezeSpeed, between kLeastSway and kMostSway times that.
 * Each material's foliage settings scale it for its own stiffness.
 *
 * @param wind the wind
 * @return its sway
 */
float windSway(const Wind& wind);

} // namespace sfs
