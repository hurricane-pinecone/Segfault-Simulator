#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include <cstdint>

namespace sfs
{

// One stamped piece of a splatter: a decal footprint (size.x = length along
// `rotation`'s local +X, size.y = width across) and its orientation.
struct SplatShape
{
  glm::vec2 size{0.0f, 0.0f};
  float rotation = 0.0f;
};

// A small, heap-free set of shapes for one impact: a main splat plus optional
// fanned sub-streaks. Built per particle collision, so it deliberately avoids an
// allocation.
struct SplatPattern
{
  static constexpr int kMax = 3; // main + up to 2 fanned streaks
  SplatShape shapes[kMax];
  int count = 0;
};

// Shared splatter shaping used by both the isometric and flat decal paths: turn
// an impact into a directional, speed-scaled set of decal shapes. `vel`/`velZ`
// is the impact velocity (any units), `baseSize` the base footprint, `refSpeed`
// the impact speed at which elongation/fan saturate (SAME units as `vel` -- a few
// tiles/sec on the iso path, a few hundred px/sec on the flat path), and `fan`
// adds sub-streaks (e.g. on solid ground, off for water/wall). A near-zero
// planar velocity falls back to a random orientation (a round drop reads the same
// at any angle).
SplatPattern buildSplatShapes(glm::vec2 vel,
                              float velZ,
                              float baseSize,
                              float refSpeed,
                              bool fan,
                              std::uint32_t& rng);

} // namespace sfs
