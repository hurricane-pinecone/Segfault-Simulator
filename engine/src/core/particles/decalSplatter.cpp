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

// Built-in topology: a soft round pool plus crisp directional fan streaks.
class PoolStreakShaper : public ISplatterShaper
{
public:
  SplatPattern shape(const SplatImpact& impact,
                     const SplatParams& sp,
                     std::uint32_t& rng) const override
  {
    const float planar = glm::length(impact.velocity);
    const float mag = glm::sqrt(planar * planar + impact.velZ * impact.velZ);
    const float ref = sp.refSpeed > 0.0001f ? sp.refSpeed : 1.0f;

    // How directional the hit was (horizontal travel) and its overall force.
    // The 1.6 keeps energy a little behind drift, so a hit elongates before it
    // fans.
    const float drift = glm::clamp(planar / ref, 0.0f, 1.0f);
    const float energy = glm::clamp(mag / (ref * 1.6f), 0.0f, 1.0f);
    const float base = planar > 0.05f
                           ? glm::atan(impact.velocity.y, impact.velocity.x)
                           : randf(rng) * 6.2831853f;

    SplatPattern out;

    // Pool: a round blob (orientation irrelevant; kept square).
    out.shapes[out.count++] = {
        glm::vec2{impact.baseSize, impact.baseSize}, base, !sp.poolSoft};

    if (!sp.fan || energy <= 0.15f || sp.streakMaxCount <= 0)
      return out;

    // Fanned streaks: tight around the drift when directional, splayed wide
    // when the hit came mostly straight down. Count scales with impact energy.
    const int streaks = glm::clamp(
        1 + static_cast<int>(energy * static_cast<float>(sp.streakMaxCount)),
        1,
        sp.streakMaxCount);
    const float spread = glm::mix(1.4f, 0.45f, drift);
    for (int k = 0; k < streaks && out.count < SplatPattern::kMax; ++k)
    {
      const float a = base + (randf(rng) - 0.5f) * 2.0f * spread;
      const float len = impact.baseSize * (1.0f + energy * 3.0f) *
                        sp.streakLengthScale * (0.6f + randf(rng) * 0.6f);
      out.shapes[out.count++] = {
          glm::vec2{len, sp.streakWidth}, a, sp.streakCrisp};
    }

    return out;
  }
};
} // namespace

const ISplatterShaper& defaultSplatterShaper()
{
  static const PoolStreakShaper shaper;
  return shaper;
}

} // namespace sfs
