#pragma once
#include <sfs/sfs.h>

// A hovering drone: bobs around `home`. Dash through it or land on it to
// destroy it (a combo step); touch it any other way and the run ends.
struct Drone
{
  glm::vec2 home{0.0f, 0.0f};
  float amplitude = 48.0f; // how far it bobs
  float speed = 2.5f;      // bobs a second (radians)
  float clock = 0.0f;
};
