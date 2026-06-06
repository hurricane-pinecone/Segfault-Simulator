#include "engine/core/particles/decalSplatter.h"

#include "glm/glm/common.hpp"
#include "glm/glm/exponential.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/trigonometric.hpp"

namespace sfs
{

namespace
{
// xorshift32 in [0,1), matching the particle engine's RNG so shaping draws stay
// reproducible per emitter.
float randf(std::uint32_t& state)
{
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return static_cast<float>(state & 0xFFFFFFu) / static_cast<float>(0x1000000);
}
} // namespace

SplatPattern buildSplatShapes(glm::vec2 vel,
                              float velZ,
                              float baseSize,
                              float refSpeed,
                              bool fan,
                              std::uint32_t& rng)
{
  const float planar = glm::length(vel);
  const float impact = glm::sqrt(planar * planar + velZ * velZ);
  const float ref = refSpeed > 0.0001f ? refSpeed : 1.0f;

  // How directional the hit was (horizontal travel) and its overall force. The
  // 1.6 keeps energy a little behind drift, so a hit elongates before it fans.
  const float drift = glm::clamp(planar / ref, 0.0f, 1.0f);
  const float energy = glm::clamp(impact / (ref * 1.6f), 0.0f, 1.0f);
  const float base =
      planar > 0.05f ? glm::atan(vel.y, vel.x) : randf(rng) * 6.2831853f;

  SplatPattern out;

  // Main splat: round for a near-vertical drop, a teardrop along travel.
  out.shapes[out.count++] = {
      glm::vec2{baseSize * (1.0f + drift * 2.2f), baseSize}, base};

  if (!fan || energy <= 0.15f)
    return out;

  // Fanned sub-streaks: tight around the drift when directional, splayed wide
  // when the hit came mostly straight down.
  const int streaks = glm::clamp(1 + static_cast<int>(energy * 2.5f), 1, 2);
  const float spread = glm::mix(1.4f, 0.45f, drift);
  for (int k = 0; k < streaks && out.count < SplatPattern::kMax; ++k)
  {
    const float a = base + (randf(rng) - 0.5f) * 2.0f * spread;
    const float len =
        baseSize * (1.6f + energy * 3.0f) * (0.6f + randf(rng) * 0.6f);
    const float wid = baseSize * (0.25f + randf(rng) * 0.2f);
    out.shapes[out.count++] = {glm::vec2{len, wid}, a};
  }

  return out;
}

} // namespace sfs
