#pragma once

#include <glm/glm/ext/vector_float3.hpp>

namespace sfs
{

/**
 * The scene's directional light, as a directional light in Unity, Unreal or
 * Godot: the sun by day, the moon by night (less intense), as a game's
 * day/night cycle writes it each frame. The renderer reads it each frame, in
 * every style. A scene without one has no directional light: no light and
 * no shadows from it. The sky's clouds (Clouds) dim it on their own.
 *
 * Under a physical sky the sky works the light out itself from where its sun
 * and moon are (CelestialBodies; without them, a sun along `direction`):
 * their colour through the air and their brightness are the sky's.
 */
struct [[sfs::scene_data]] DirectionalLighting
{
  /**
   * Points toward the light (need not be unit length). Z is up, so Z <= 0
   * means it's below the horizon: no light, no shadows. A 2D game's is in its
   * own space: x and y across its plane (y down the screen if it runs down),
   * z up from it (toward the viewer, in a flat game).
   */
  glm::vec3 direction{-0.45f, -0.35f, 0.82f};
  /** Its tint (linear RGB). */
  [[sfs::color]] glm::vec3 color{1.0f, 0.96f, 0.88f};
  /**
   * How much a surface facing it brightens over the ambient light (0..1; a
   * full moon's is about 0.12).
   */
  [[sfs::range(0, 1)]] float intensity = 0.85f;
};

} // namespace sfs
