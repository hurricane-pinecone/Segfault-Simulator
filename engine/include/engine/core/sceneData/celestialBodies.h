#pragma once

#include <glm/glm/ext/vector_float3.hpp>

namespace sfs
{

/**
 * Where a physical sky's sun and moon are, and how full the moon is, as a
 * game's day/night cycle writes them each frame (Unity HDRP's celestial
 * bodies). The sun lights the sky's air (by day, and its glow through
 * twilight, below the horizon); both are drawn as discs, the moon's lit to
 * its phase; the night sky glows brighter under the moon, and once the sun has
 * set the moon is the light that shines and casts shadows, fading in as the
 * sun sinks (moonlight()). A physical sky works the directional light out
 * from these. A flat sky draws neither: its light is the scene's
 * DirectionalLighting as it is. A 3D scene's.
 */
struct [[sfs::scene_data]] CelestialBodies
{
  /** Points toward the sun (need not be unit length); Z <= 0: it has set. */
  glm::vec3 sunDirection{-0.45f, -0.35f, 0.82f};
  /**
   * Points toward the moon (need not be unit length); Z <= 0: below the
   * horizon (the default: no moon).
   */
  glm::vec3 moonDirection{0.0f, 0.0f, -1.0f};
  /** How much of the moon's face is lit: 0 new, 0.5 a half moon, 1 full. */
  [[sfs::range(0, 1)]] float moonPhase = 1.0f;
};

} // namespace sfs
