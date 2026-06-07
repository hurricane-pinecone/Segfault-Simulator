#include "../../../testHarness.h"

#include <engine/core/rendering/batchKeys/LitQuadBatchKey.h>
#include <engine/core/rendering/batchKeys/ParticleBatchKey.h>
#include <engine/core/rendering/quads.h>
#include <engine/core/types/blendMode.h>

using namespace sfs;

namespace
{
LitQuad litQuad(unsigned int texture)
{
  LitQuad q;
  q.texture = texture;
  q.lightIntensity = 1.0f;
  q.ambient = 0.18f;
  q.diffuseStrength = 0.85f;
  return q;
}
} // namespace

int main()
{
  TEST("a lit batch key should fall back to the default normal map")
  {
    LitQuad q = litQuad(3); // hasNormalMap defaults false
    const LitBatchKey key = LitBatchKey::from(q, 99);
    CHECK(key.texture == 3);
    CHECK(key.normalTexture == 99); // the supplied default
    CHECK(key.hasNormalMap == false);
  }

  TEST("a lit batch key should keep its own normal map when present")
  {
    LitQuad q = litQuad(3);
    q.hasNormalMap = true;
    q.normalTexture = 7;
    const LitBatchKey key = LitBatchKey::from(q, 99);
    CHECK(key.normalTexture == 7); // its own, not the default
    CHECK(key.hasNormalMap == true);
  }

  TEST("lit batch keys should compare equal only when every field matches")
  {
    const LitBatchKey a = LitBatchKey::from(litQuad(3), 0);
    const LitBatchKey b = LitBatchKey::from(litQuad(3), 0);
    CHECK(a == b);

    const LitBatchKey c = LitBatchKey::from(litQuad(4), 0); // different texture
    CHECK(!(a == c));

    LitQuad litErringEffect = litQuad(3);
    litErringEffect.surfaceEffect = 2;
    const LitBatchKey d = LitBatchKey::from(litErringEffect, 0);
    CHECK(!(a == d)); // different surface effect
  }

  TEST("particle batch keys should order by texture, then blend, then depth")
  {
    const ParticleBatchKey base{2, BlendMode::Alpha, true};

    ParticleBatchKey lowerTexture{1, BlendMode::Alpha, true};
    CHECK(lowerTexture < base); // texture dominates

    ParticleBatchKey sameTextureLowerBlend{2, BlendMode::Additive, true};
    // Additive vs Alpha ordering is by the enum value; the key just needs a
    // strict weak ordering, so exactly one direction holds.
    CHECK((sameTextureLowerBlend < base) != (base < sameTextureLowerBlend));

    ParticleBatchKey identical{2, BlendMode::Alpha, true};
    CHECK(!(identical < base)); // equal keys: neither orders before the other
    CHECK(!(base < identical));
  }

  return testing::report("batchKeyTests");
}
