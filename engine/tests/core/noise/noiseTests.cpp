// Tests for sfs::Noise: determinism under a fixed seed, a sensible value range,
// seed/type sensitivity, and move semantics.

#include "../../testHarness.h"

#include <engine/core/noise/noise.h>

#include <cmath>
#include <utility>

using namespace sfs;

int main()
{
  // --- determinism: same seed + coords -> identical values ----------------
  {
    Noise a;
    a.setSeed(1234);
    a.setFrequency(0.1f);
    Noise b;
    b.setSeed(1234);
    b.setFrequency(0.1f);
    for (int i = 0; i < 20; ++i)
    {
      const float x = i * 1.3f;
      const float y = i * -0.7f;
      CHECK(testing::approx(a.get(x, y), b.get(x, y), 1e-6f));
    }
  }

  // --- value stays within roughly [-1, 1] and is not constant -------------
  {
    Noise n;
    n.setSeed(99);
    n.setFrequency(0.25f);
    const float first = n.get(0.0f, 0.0f);
    bool varied = false;
    for (int y = 0; y < 32; ++y)
      for (int x = 0; x < 32; ++x)
      {
        const float v = n.get(x * 0.5f, y * 0.5f);
        CHECK(v >= -1.0001f && v <= 1.0001f);
        if (std::fabs(v - first) > 1e-3f)
          varied = true;
      }
    CHECK(varied);
  }

  // --- different seeds generally produce different fields -----------------
  {
    Noise a;
    a.setSeed(1);
    a.setFrequency(0.1f);
    Noise b;
    b.setSeed(2);
    b.setFrequency(0.1f);
    int differences = 0;
    for (int i = 0; i < 50; ++i)
      if (!testing::approx(
              a.get(i * 0.9f, i * 0.3f), b.get(i * 0.9f, i * 0.3f), 1e-4f))
        ++differences;
    CHECK(differences > 0);
  }

  // --- Perlin type is also deterministic ----------------------------------
  {
    Noise a;
    a.setType(Noise::Type::Perlin);
    a.setSeed(7);
    a.setFrequency(0.2f);
    Noise b;
    b.setType(Noise::Type::Perlin);
    b.setSeed(7);
    b.setFrequency(0.2f);
    CHECK(testing::approx(a.get(3.0f, 4.0f), b.get(3.0f, 4.0f), 1e-6f));
  }

  // --- move construction preserves output ---------------------------------
  {
    Noise a;
    a.setSeed(55);
    a.setFrequency(0.15f);
    const float before = a.get(2.0f, 2.0f);
    Noise moved = std::move(a);
    CHECK(testing::approx(moved.get(2.0f, 2.0f), before, 1e-6f));
  }

  return testing::report("noiseTests");
}
