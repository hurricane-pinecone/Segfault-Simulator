#pragma once

#include <glm/glm/ext/vector_float3.hpp>

namespace sfs
{

/**
 * The light from all around, as an ambient or sky light in Unity, Unreal or
 * Godot: how much of it everything gets, and the sky's colour (what the
 * background, haze and shiny things' reflections show, under a flat sky). The
 * renderer reads it each frame, in every style. A scene without one has none:
 * no light but its sun, moon and lamps, and a black sky.
 */
struct [[sfs::scene_data]] AmbientLighting
{
  /**
   * The base brightness everything receives (0: black night; 1: fully lit).
   * Point lights fade in as it drops.
   */
  [[sfs::range(0, 1)]] float intensity = 0.55f;
  /** The sky's colour (linear RGB), under a flat sky. */
  [[sfs::color]] glm::vec3 skyColor{0.55f, 0.72f, 0.90f};
};

} // namespace sfs
