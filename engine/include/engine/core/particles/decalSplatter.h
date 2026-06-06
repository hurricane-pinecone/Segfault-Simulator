#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include <cstdint>

namespace sfs
{

// One stamped piece of a splatter: a decal footprint (size.x = length along
// `rotation`'s local +X, size.y = width across), its orientation, and whether it
// reads crisp (hard-edged streak) or soft (the sink chooses the sprite/sampling).
struct SplatShape
{
  glm::vec2 size{0.0f, 0.0f};
  float rotation = 0.0f;
  bool crisp = false;
};

// A small, heap-free set of marks for one impact. Built per particle collision,
// so it avoids an allocation; kMax bounds how many marks a shaper may emit.
struct SplatPattern
{
  static constexpr int kMax = 16;
  SplatShape shapes[kMax];
  int count = 0;
};

// Tuning for the default shaper, mirrored from DecalSpec. Units match the
// effect's velocity scale (tiles/sec iso, px/sec flat).
struct SplatParams
{
  float refSpeed = 4.0f;          // impact speed for full elongation / fan
  int streakMaxCount = 2;         // max fan streaks (scaled by impact energy)
  float streakWidth = 0.03f;      // streak thickness
  float streakLengthScale = 1.0f; // multiplies the speed-driven length
  bool fan = true;                // emit streaks at all (off for water, say)
  bool poolSoft = true;           // pool reads soft (else crisp)
  bool streakCrisp = true;        // streaks read crisp (else soft)
};

// One impact handed to a shaper. `velocity` is the in-plane impact velocity
// (path-mapped: world XY on the ground, along-edge + vertical on a wall); `velZ`
// is any out-of-plane vertical; `baseSize` the sampled pool footprint.
struct SplatImpact
{
  glm::vec2 velocity{0.0f, 0.0f};
  float velZ = 0.0f;
  float baseSize = 0.15f;
};

// Strategy that turns an impact into the set of decal marks -- the splatter
// TOPOLOGY. Implement this for fully custom splatter (rings, cross-hatches,
// stamps, whatever) and point a DecalSpec at it; the engine ships
// defaultSplatterShaper() (a soft round pool + crisp directional fan streaks),
// which both samples use. The shaper produces relative shapes; the engine places
// them on the surface (elevation, wall face, clipping). A near-zero planar
// velocity should fall back to a random orientation (a round drop reads the same
// at any angle).
class ISplatterShaper
{
public:
  virtual ~ISplatterShaper() = default;
  virtual SplatPattern shape(const SplatImpact& impact,
                             const SplatParams& params,
                             std::uint32_t& rng) const = 0;
};

// The built-in shaper: a soft round pool (shapes[0]) plus crisp directional fan
// streaks. Returned when a DecalSpec leaves its shaper null.
const ISplatterShaper& defaultSplatterShaper();

} // namespace sfs
