#include "../../testHarness.h"

#include <engine/core/particles/decalSplatter.h>

#include <cstdint>

using namespace sfs;

namespace
{
SplatParams defaultParams()
{
  SplatParams sp;
  sp.refSpeed = 4.0f;
  sp.streakMaxCount = 2;
  sp.fan = true;
  sp.poolSoft = true;
  sp.streakCrisp = true;
  return sp;
}
} // namespace

int main()
{
  const ISplatterShaper& shaper = defaultSplatterShaper();

  TEST("an energetic directional hit should emit a pool plus fan streaks")
  {
    SplatParams sp = defaultParams();
    std::uint32_t rng = 12345u;
    SplatImpact impact;
    impact.velocity = {20.0f, 0.0f}; // well above refSpeed -> full energy
    impact.baseSize = 0.15f;

    const SplatPattern p = shaper.shape(impact, sp, rng);
    CHECK(p.count >= 1 && p.count <= SplatPattern::kMax);
    CHECK(p.count == 3); // pool + streakMaxCount(2) streaks at full energy

    // shapes[0] is the pool: square footprint, soft (poolSoft -> not crisp)
    CHECK(testing::approx(p.shapes[0].size.x, 0.15f));
    CHECK(testing::approx(p.shapes[0].size.y, 0.15f));
    CHECK(p.shapes[0].crisp == false);

    CHECK(p.shapes[1].crisp == true); // streaks are crisp
  }

  TEST("a still drop should emit only a pool")
  {
    SplatParams sp = defaultParams();
    std::uint32_t rng = 999u;
    SplatImpact drop;
    drop.velocity = {0.0f, 0.0f};
    drop.baseSize = 0.2f;

    const SplatPattern p = shaper.shape(drop, sp, rng);
    CHECK(p.count == 1);
  }

  TEST("a disabled fan should emit only a pool even for a fast hit")
  {
    SplatParams sp = defaultParams();
    sp.fan = false;
    std::uint32_t rng = 7u;
    SplatImpact impact;
    impact.velocity = {20.0f, 0.0f};
    impact.baseSize = 0.15f;

    const SplatPattern p = shaper.shape(impact, sp, rng);
    CHECK(p.count == 1);
  }

  TEST("poolSoft=false should make the pool crisp")
  {
    SplatParams sp = defaultParams();
    sp.poolSoft = false;
    std::uint32_t rng = 3u;
    SplatImpact drop;
    drop.velocity = {0.0f, 0.0f};
    drop.baseSize = 0.1f;

    const SplatPattern p = shaper.shape(drop, sp, rng);
    CHECK(p.shapes[0].crisp == true);
  }

  return testing::report("decalSplatterTests");
}
