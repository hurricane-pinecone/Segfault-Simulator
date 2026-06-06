// Tests for the render-context-free ParticleEngine: effect registration, the
// fire-and-forget burst path, lifetime decay, the global particle cap, and
// projecting live particles into render batches through a stub IProjection.

#include "../../testHarness.h"

#include <engine/core/particles/particleEngine.h>
#include <engine/core/rendering/iProjection.h>

#include <string>
#include <vector>

using namespace sfs;

namespace
{
// Identity projection so a world position maps straight to a screen pixel and a
// world unit is one pixel -- enough to drive buildBatches in isolation.
struct StubProjection : IProjection
{
  glm::vec2 worldToScreen(const glm::vec2& world, float) const override
  {
    return world;
  }
  glm::vec2 screenToWorld(const glm::vec2& screen, float) const override
  {
    return screen;
  }
  float worldUnitToPixels() const override { return 1.0f; }
};

ParticleEffectDesc burstEffect(int count, float lifetime)
{
  ParticleEffectDesc d;
  d.burstCount = count;
  d.lifetime = FloatRange::of(lifetime);
  d.speed = FloatRange::of(0.0f);
  d.space = SimulationSpace::World;
  d.ground = GroundBehavior::None;
  return d;
}
} // namespace

int main()
{
  // --- register / has / effect / effectNames ------------------------------
  {
    ParticleEngine engine;
    CHECK(!engine.hasEffect("spark"));
    CHECK(engine.effect("spark") == nullptr);

    engine.registerEffect("spark", burstEffect(5, 1.0f));
    engine.registerEffect("smoke", burstEffect(3, 2.0f));
    CHECK(engine.hasEffect("spark"));
    CHECK(engine.effect("spark") != nullptr);

    // an empty texture resolves to the built-in shape texture at register time
    CHECK(engine.effect("spark")->texture == std::string("white_dot"));

    const std::vector<std::string> names = engine.effectNames();
    CHECK(names.size() == 2);
    CHECK(names[0] == "smoke"); // effectNames() is sorted
    CHECK(names[1] == "spark");

    // re-registering a name replaces rather than duplicates
    engine.registerEffect("spark", burstEffect(9, 1.0f));
    CHECK(engine.effectNames().size() == 2);
    CHECK(engine.effect("spark")->burstCount == 9);
  }

  // --- spawnBurst: unknown effect fails, known one emits on simulate ------
  {
    ParticleEngine engine;
    engine.registerEffect("spark", burstEffect(5, 1.0f));

    CHECK(!engine.spawnBurst("does-not-exist", {0.0f, 0.0f}));
    CHECK(engine.spawnBurst("spark", {0.0f, 0.0f}));

    // burst particles materialise on the next simulate(), not at spawn time
    CHECK(engine.liveParticleCount() == 0);
    engine.simulate(0.1);
    CHECK(engine.liveParticleCount() == 5);
  }

  // --- particles age out and the burst drains -----------------------------
  {
    ParticleEngine engine;
    engine.registerEffect("spark", burstEffect(4, 1.0f));
    engine.spawnBurst("spark", {0.0f, 0.0f});
    engine.simulate(0.1);
    CHECK(engine.liveParticleCount() == 4);
    engine.simulate(2.0); // past the 1s lifetime
    CHECK(engine.liveParticleCount() == 0);
  }

  // --- global particle cap drops over-budget emissions --------------------
  {
    ParticleEngine engine;
    engine.setMaxParticles(3);
    engine.registerEffect("spark", burstEffect(10, 1.0f));
    engine.spawnBurst("spark", {0.0f, 0.0f});
    engine.simulate(0.1);
    CHECK(engine.liveParticleCount() == 3);
  }

  // --- buildBatches projects live particles into one batch ----------------
  {
    ParticleEngine engine;
    engine.registerEffect("spark", burstEffect(6, 1.0f));
    engine.spawnBurst("spark", {2.0f, 3.0f});
    engine.simulate(0.1);

    StubProjection proj;
    std::vector<ParticleRenderBatch> batches;
    engine.buildBatches(proj, batches);
    CHECK(batches.size() == 1);
    CHECK(batches[0].geometry.quads.size() == 6);
    CHECK(!batches[0].screenSpace);
    CHECK(batches[0].blend == BlendMode::Alpha);

    // with no live particles, no batch is emitted
    engine.simulate(5.0);
    std::vector<ParticleRenderBatch> empty;
    engine.buildBatches(proj, empty);
    CHECK(empty.empty());
  }

  // --- additive blend carried through to the batch ------------------------
  {
    ParticleEngine engine;
    ParticleEffectDesc d = burstEffect(2, 1.0f);
    d.blend = BlendMode::Additive;
    engine.registerEffect("glow", d);
    engine.spawnBurst("glow", {0.0f, 0.0f});
    engine.simulate(0.1);

    StubProjection proj;
    std::vector<ParticleRenderBatch> batches;
    engine.buildBatches(proj, batches);
    CHECK(batches.size() == 1);
    CHECK(batches[0].blend == BlendMode::Additive);
  }

  return testing::report("particleEngineTests");
}
