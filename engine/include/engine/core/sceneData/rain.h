#pragma once

namespace sfs
{

/**
 * The rain falling: how hard. The renderer reads it each frame for the rain
 * it draws (its streaks, splashes, ripples, drips and veil; how they look is
 * RainSettings), slanted by the scene's Wind. A scene without it is dry. A
 * 3D scene's.
 */
struct [[sfs::scene_data]] Rain
{
  /** How hard it's raining (0: not; 1: a downpour). */
  [[sfs::range(0, 1)]] float intensity = 0.0f;
};

} // namespace sfs
