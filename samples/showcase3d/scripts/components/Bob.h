#pragma once
#include <sfs/sfs.h>

// Floats (and spins) about where it was placed: along `axis`, `amplitude`
// metres each way, `speed` times a second, turning `spin` radians a second
// around Z. On a kinematic body it's a moving platform.
struct Bob
{
  glm::vec3 axis{0.0f, 0.0f, 1.0f};
  [[sfs::range(0, 10)]] float amplitude = 0.25f;
  [[sfs::range(0, 5)]] float speed = 1.0f;
  [[sfs::range(-10, 10)]] float spin = 0.0f;
  // Where it was placed (set as it starts), and how long it's been going.
  [[sfs::hide]] glm::vec3 home{0.0f};
  [[sfs::hide]] glm::quat facing{1.0f, 0.0f, 0.0f, 0.0f};
  [[sfs::hide]] float clock = -1.0f;
};
