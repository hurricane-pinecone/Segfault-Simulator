#pragma once

namespace sfs
{

/**
 * The sky's clouds: how much of it they cover and how heavy they are. The
 * renderer reads them each frame. They dim the directional light (the
 * scene's DirectionalLighting, or a physical sky's sun or moon) and soften
 * shadows, the more the more there are; under a physical sky they're drawn,
 * a layer about 1.5 km up drifting on the scene's Wind, casting moving
 * shadows. A scene without them has a clear sky. A 3D scene's.
 */
struct [[sfs::scene_data]] Clouds
{
  /**
   * How much of the sky they cover (0: clear; 1: overcast): scattered cumulus
   * when low, broken cloud in the middle, a grey deck near 1.
   */
  [[sfs::range(0, 1)]] float cover = 0.0f;
  /**
   * How heavy they are (0: white fair-weather cloud; 1: dark rain cloud):
   * darker undersides, and less of the sun gets through.
   */
  [[sfs::range(0, 1)]] float darkness = 0.0f;
};

} // namespace sfs
