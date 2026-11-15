#include "../../testHarness.h"

#include <engine/core/components/particleEmitterComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/components/windZoneComponent.h>
#include <engine/core/ecs/registry.h>
#include <engine/core/particles/particleEngine.h>

#include <cmath>

#include <string>
#include <vector>

using namespace sfs;

namespace
{
ParticleEffectDesc burstEffect(int count, float lifetime)
{
  ParticleEffectDesc d;
  d.burstCount = count;
  d.lifetime = FloatRange::of(lifetime);
  d.speed = FloatRange::of(0.0f);
  d.ground = GroundBehavior::None;
  return d;
}
// One particle that stays where it's made, carried by the wind toward
// `influence` times it, through drag 2.
ParticleEffectDesc driftEffect(float influence)
{
  ParticleEffectDesc d = burstEffect(1, 100.0f);
  d.drag = 2.0f;
  d.windInfluence = influence;
  return d;
}

// Where the one live particle is.
glm::vec2 onlyParticle(const ParticleEngine& engine)
{
  std::vector<ParticleSpriteBatch> batches;
  engine.sprites(batches);
  return batches.empty() || batches[0].sprites.empty()
             ? glm::vec2(NAN)
             : batches[0].sprites[0].position;
}

// The one particle's speed after `seconds` of 10 ms steps, from its last step.
glm::vec2 settledVelocity(ParticleEngine& engine, float seconds)
{
  constexpr double kStep = 0.01;
  for (double t = 0.0; t < seconds; t += kStep)
    engine.simulate(kStep);
  const glm::vec2 before = onlyParticle(engine);
  engine.simulate(kStep);
  return (onlyParticle(engine) - before) / static_cast<float>(kStep);
}

Entity windZone(Registry& registry, const glm::vec2& wind)
{
  Entity zone = registry.createEntity();
  zone.addComponent<WindZoneComponent>(WindZoneComponent{wind, true});
  return zone;
}
} // namespace

int main()
{
  TEST("effects should register, replace by name, and list sorted")
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

  TEST("spawnBurst should reject an unknown effect and emit on simulate")
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

  TEST("particles should age out and drain the burst")
  {
    ParticleEngine engine;
    engine.registerEffect("spark", burstEffect(4, 1.0f));
    engine.spawnBurst("spark", {0.0f, 0.0f});
    engine.simulate(0.1);
    CHECK(engine.liveParticleCount() == 4);
    engine.simulate(2.0); // past the 1s lifetime
    CHECK(engine.liveParticleCount() == 0);
  }

  TEST("the global cap should drop over-budget emissions")
  {
    ParticleEngine engine;
    engine.setMaxParticles(3);
    engine.registerEffect("spark", burstEffect(10, 1.0f));
    engine.spawnBurst("spark", {0.0f, 0.0f});
    engine.simulate(0.1);
    CHECK(engine.liveParticleCount() == 3);
  }

  TEST("sprites should hand out the live particles, an effect a batch")
  {
    ParticleEngine engine;
    engine.registerEffect("spark", burstEffect(6, 1.0f));
    engine.spawnBurst("spark", {2.0f, 3.0f});
    engine.simulate(0.1);

    std::vector<ParticleSpriteBatch> batches;
    engine.sprites(batches);
    CHECK(batches.size() == 1);
    CHECK(batches[0].sprites.size() == 6);
    CHECK(testing::approx(batches[0].sprites[0].position.x, 2.0f));
    CHECK(batches[0].blend == BlendMode::Alpha);

    // with no live particles, no batch is handed out
    engine.simulate(5.0);
    std::vector<ParticleSpriteBatch> empty;
    engine.sprites(empty);
    CHECK(empty.empty());
  }

  TEST("an additive effect should carry its blend mode to the batch")
  {
    ParticleEngine engine;
    ParticleEffectDesc d = burstEffect(2, 1.0f);
    d.blend = BlendMode::Additive;
    engine.registerEffect("glow", d);
    engine.spawnBurst("glow", {0.0f, 0.0f});
    engine.simulate(0.1);

    std::vector<ParticleSpriteBatch> batches;
    engine.sprites(batches);
    CHECK(batches.size() == 1);
    CHECK(batches[0].blend == BlendMode::Additive);
  }

  TEST("an emitter's shape scale should stretch where a box spawns, and its "
       "cap override the effect's")
  {
    Registry registry;
    ParticleEngine engine;
    engine.setRegistry(&registry);
    ParticleEffectDesc d = burstEffect(0, 10.0f);
    d.shape = EmissionShape::Box;
    d.boxExtents = {1.0f, 2.0f};
    d.emitRate = 2000.0f;
    d.maxParticles = 512;
    engine.registerEffect("dust", d);

    Entity entity = registry.createEntity();
    entity.addComponent<TransformComponent>();
    ParticleEmitterComponent emitter("dust");
    emitter.shapeScale = {3.0f, 0.5f};
    entity.addComponent<ParticleEmitterComponent>(emitter);
    registry.flushEntities();
    engine.simulate(0.1);

    std::vector<ParticleSpriteBatch> batches;
    engine.sprites(batches);
    CHECK(batches.size() == 1 && batches[0].sprites.size() == 200);
    bool within = true;
    float widest = 0.0f;
    for (const ParticleSprite& sprite : batches[0].sprites)
    {
      within = within && std::abs(sprite.position.x) <= 3.0f + 1e-4f &&
               std::abs(sprite.position.y) <= 1.0f + 1e-4f;
      widest = std::max(widest, std::abs(sprite.position.x));
    }
    CHECK(within);
    CHECK(widest > 2.0f); // past the effect's own extent

    auto& own = *entity.getComponent<ParticleEmitterComponent>();
    own.maxParticles = 600;
    engine.simulate(0.3);
    CHECK(engine.liveParticleCount() == 600); // over the effect's 512
    own.maxParticles = 0;
    ParticleEffectDesc capped = d;
    capped.maxParticles = 20;
    engine.registerEffect("capped", capped);
    own.effect = "capped";
    own.maxParticles = 50;
    engine.simulate(0.1);
    CHECK(engine.liveParticleCount() == 50);
  }

  TEST("a particle with drag d and wind influence k should settle at k times "
       "the wind")
  {
    Registry registry;
    windZone(registry, {5.0f, 0.0f});
    registry.flushEntities();
    ParticleEngine engine;
    engine.setRegistry(&registry);
    engine.registerEffect("chaff", driftEffect(0.5f));
    engine.spawnBurst("chaff", {0.0f, 0.0f});
    const glm::vec2 velocity = settledVelocity(engine, 5.0f);
    // A step's damping keeps it a hair (d * dt) under.
    CHECK(testing::approx(velocity.x, 2.5f, 0.07f));
    CHECK(testing::approx(velocity.y, 0.0f, 1e-4f));
  }

  TEST("with no wind zone, or no influence, a particle should run as before")
  {
    Registry registry;
    ParticleEngine engine;
    engine.setRegistry(&registry);
    engine.registerEffect("chaff", driftEffect(0.5f));
    engine.spawnBurst("chaff", {1.0f, 2.0f});
    CHECK(glm::length(settledVelocity(engine, 1.0f)) < 1e-4f);

    Registry windy;
    windZone(windy, {5.0f, 0.0f});
    windy.flushEntities();
    ParticleEngine still;
    still.setRegistry(&windy);
    still.registerEffect("chaff", driftEffect(0.0f));
    still.spawnBurst("chaff", {1.0f, 2.0f});
    CHECK(glm::length(settledVelocity(still, 1.0f)) < 1e-4f);
  }

  TEST("two wind zones should add, and a disabled one blow nothing")
  {
    Registry registry;
    windZone(registry, {3.0f, 0.0f});
    windZone(registry, {0.0f, 4.0f});
    Entity off = windZone(registry, {50.0f, 50.0f});
    off.getComponent<WindZoneComponent>()->enabled = false;
    registry.flushEntities();
    ParticleEngine engine;
    engine.setRegistry(&registry);
    engine.registerEffect("chaff", driftEffect(1.0f));
    engine.spawnBurst("chaff", {0.0f, 0.0f});
    const glm::vec2 velocity = settledVelocity(engine, 5.0f);
    CHECK(testing::approx(velocity.x, 3.0f, 0.08f));
    CHECK(testing::approx(velocity.y, 4.0f, 0.1f));
  }

  return testing::report("particleEngineTests");
}
