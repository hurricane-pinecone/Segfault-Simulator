#pragma once
#include <sfs/sfs.h>

// Touching it ends the run (spikes). `half` is its hurt box's half-size,
// centred `offset` from the entity.
struct Hazard
{
  glm::vec2 half{24.0f, 14.0f};
  glm::vec2 offset{0.0f, 0.0f};
};
