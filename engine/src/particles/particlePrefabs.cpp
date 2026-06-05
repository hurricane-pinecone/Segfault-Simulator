#include "engine/particles/particlePrefabs.h"

#include "engine/rendering/modules/particles.h"

namespace sfs
{

// Layer 1: fine, bright, fast-fading additive haze right at the impact.
ParticleEffectDesc bloodMistEffect()
{
  ParticleEffectDesc d;

  d.texture = "white_dot";
  d.blend = BlendMode::Additive; // bright pink haze
  d.space = SimulationSpace::World;

  d.shape = EmissionShape::Circle;
  d.shapeRadius = 0.05f;
  d.burstCount = 20;

  d.lifetime = FloatRange::of(0.12f, 0.34f); // gone almost immediately
  d.speed = FloatRange::of(2.0f, 5.0f);
  d.launchHeightSpeed = FloatRange::of(1.0f, 3.0f);
  d.startHeight = FloatRange::of(0.1f, 0.5f);
  d.size = FloatRange::of(0.04f, 0.11f);
  d.directionSpread = 1.4f; // a touch wider than the spray -- it's a cloud

  d.gravityZ = -3.0f; // barely falls
  d.drag = 4.5f;      // slows fast and hangs

  d.colorOverLife = Gradient::twoStop(glm::vec3{1.0f, 0.3f, 0.3f},
                                      glm::vec3{0.4f, 0.04f, 0.04f});
  d.alphaOverLife = Curve{}.add(0.0f, 0.0f).add(0.1f, 0.7f).add(1.0f, 0.0f);
  d.sizeOverLife = Curve{}.add(0.0f, 0.6f).add(1.0f, 1.4f); // expands

  d.ground = GroundBehavior::None;
  d.maxParticles = 128;
  return d;
}

// Layer 2: the main directional droplet spray (slimmed-down particle count).
ParticleEffectDesc bloodSprayEffect()
{
  ParticleEffectDesc d;

  d.texture = "white_dot";
  d.blend = BlendMode::Alpha;
  d.space = SimulationSpace::World;

  d.shape = EmissionShape::Circle;
  d.shapeRadius = 0.06f;
  d.burstCount = 28; // minimal -- the mist + gobs carry the rest of the read

  // Droplets die on ground CONTACT (GroundBehavior::Die), not on a timer. This
  // lifetime is only a backstop for any that never hit terrain, kept long so
  // the air timer never pre-empts landing (which starved the ground of stains).
  d.lifetime = FloatRange::of(4.0f, 6.0f);
  // Scatter kept below the spawn impulse so the bulk push sets the direction.
  d.speed = FloatRange::of(1.0f, 4.0f);
  d.launchHeightSpeed = FloatRange::of(1.0f, 2.5f);
  d.startHeight = FloatRange::of(0.1f, 0.4f);
  d.size = FloatRange::of(0.05f, 0.15f);
  d.angularVelocity = FloatRange::of(-9.0f, 9.0f);
  d.directionSpread = 0.9f; // tighter forward beam

  d.gravityZ = -18.0f;
  d.drag = 3.0f; // punches out, then decelerates hard

  d.colorOverLife = Gradient::twoStop(glm::vec3{0.8f, 0.05f, 0.05f},
                                      glm::vec3{0.2f, 0.0f, 0.0f});
  // Full alpha/size through the (short) flight; the late tail only fades a rare
  // straggler that never lands. So droplets don't fade out mid-air.
  d.alphaOverLife = Curve{}.add(0.0f, 1.0f).add(0.9f, 1.0f).add(1.0f, 0.0f);
  d.sizeOverLife = Curve{}.add(0.0f, 1.0f).add(1.0f, 1.0f);

  // Land, stamp the stain, and vanish -- no lingering wet blob; the permanent
  // decal carries the mark.
  d.ground = GroundBehavior::Die;

  // Each droplet leaves a small permanent stain where it lands.
  d.leavesDecal = true;
  d.decal.texture = "white_dot";
  d.decal.size = FloatRange::of(0.07f, 0.16f);
  d.decal.useParticleColor = true;
  d.decal.alpha = 0.8f;

  d.maxParticles = 256;
  return d;
}

// Layer 3: a few big, heavy chunks. They get LESS of the impulse so they lag the
// spray, arc, and leave fat splats.
ParticleEffectDesc bloodGobsEffect()
{
  ParticleEffectDesc d;

  d.texture = "white_dot";
  d.blend = BlendMode::Alpha;
  d.space = SimulationSpace::World;

  d.shape = EmissionShape::Circle;
  d.shapeRadius = 0.05f;
  d.burstCount = 7; // just a handful of chunks

  d.lifetime = FloatRange::of(4.0f, 6.0f); // backstop; die on contact
  d.speed = FloatRange::of(1.0f, 2.5f);    // low internal scatter
  d.launchHeightSpeed = FloatRange::of(1.5f, 3.0f); // tossed up to arc
  d.startHeight = FloatRange::of(0.1f, 0.4f);
  d.size = FloatRange::of(0.14f, 0.3f); // fat gobs
  d.angularVelocity = FloatRange::of(-5.0f, 5.0f);
  d.directionSpread = 1.1f;

  d.gravityZ = -14.0f; // arc down
  d.drag = 1.2f;       // less drag -- they carry their arc

  d.colorOverLife = Gradient::twoStop(glm::vec3{0.5f, 0.0f, 0.0f},
                                      glm::vec3{0.16f, 0.0f, 0.0f});
  // Full through the flight; late tail only for a straggler that never lands.
  d.alphaOverLife = Curve{}.add(0.0f, 1.0f).add(0.9f, 1.0f).add(1.0f, 0.0f);
  d.sizeOverLife = Curve{}.add(0.0f, 1.0f).add(1.0f, 1.0f);

  // Land, stamp the splat, and vanish -- the permanent decal carries the mark.
  d.ground = GroundBehavior::Die;

  // Heavy gobs leave fat splats -- but the decal is smaller than the in-air gob
  // so it reads as a mark on the surface, not a floating blob.
  d.leavesDecal = true;
  d.decal.texture = "white_dot";
  d.decal.size = FloatRange::of(0.12f, 0.22f);
  d.decal.useParticleColor = true;
  d.decal.alpha = 0.9f;

  d.maxParticles = 96;
  return d;
}

// Layer 4: blood that just drops at the impact. Almost no outward velocity, so it
// pops up a little and falls back onto the spawn tile, staining the spot.
ParticleEffectDesc bloodDripEffect()
{
  ParticleEffectDesc d;

  d.texture = "white_dot";
  d.blend = BlendMode::Alpha;
  d.space = SimulationSpace::World;

  d.shape = EmissionShape::Circle;
  d.shapeRadius = 0.1f;
  d.burstCount = 14;

  d.lifetime = FloatRange::of(3.0f, 5.0f); // backstop; die on contact
  d.speed = FloatRange::of(0.1f, 1.0f);    // barely spreads
  d.launchHeightSpeed = FloatRange::of(0.5f, 2.4f); // small pop straight up
  d.startHeight = FloatRange::of(0.05f, 0.25f);
  d.size = FloatRange::of(0.05f, 0.12f);
  d.angularVelocity = FloatRange::of(-4.0f, 4.0f);
  d.directionSpread = 6.28318530718f; // omnidirectional little plops

  d.gravityZ = -16.0f;
  d.drag = 3.2f; // kills the little outward speed fast -> stays put

  d.colorOverLife = Gradient::twoStop(glm::vec3{0.7f, 0.03f, 0.03f},
                                      glm::vec3{0.2f, 0.0f, 0.0f});
  // Full through the flight; late tail only for a straggler that never lands.
  d.alphaOverLife = Curve{}.add(0.0f, 1.0f).add(0.9f, 1.0f).add(1.0f, 0.0f);

  // Land, stamp the spot, and vanish -- the permanent decal carries the mark.
  d.ground = GroundBehavior::Die;

  d.leavesDecal = true;
  d.decal.texture = "white_dot";
  d.decal.size = FloatRange::of(0.06f, 0.13f);
  d.decal.useParticleColor = true;
  d.decal.alpha = 0.85f;

  d.maxParticles = 128;
  return d;
}

// Warm embers: a continuous additive drift, meant for an emitter component on a
// light source. Proves additive blend + the entity-bound emitter path.
ParticleEffectDesc emberEffect()
{
  ParticleEffectDesc d;

  d.texture = "white_pixel";
  d.blend = BlendMode::Additive;
  d.space = SimulationSpace::World;

  d.shape = EmissionShape::Circle;
  d.shapeRadius = 0.12f;
  d.emitRate = 16.0f; // particles/second

  d.lifetime = FloatRange::of(0.8f, 1.9f);
  d.speed = FloatRange::of(0.1f, 0.5f);
  d.launchHeightSpeed = FloatRange::of(0.6f, 1.3f); // drift upward
  d.startHeight = FloatRange::of(0.15f, 0.45f);
  d.size = FloatRange::of(0.04f, 0.09f);

  d.gravityZ = 0.4f; // gentle rise
  d.drag = 0.6f;

  d.colorOverLife = Gradient::twoStop(glm::vec3{1.0f, 0.6f, 0.2f},
                                      glm::vec3{0.85f, 0.12f, 0.0f});
  d.alphaOverLife = Curve{}.add(0.0f, 0.0f).add(0.15f, 1.0f).add(1.0f, 0.0f);

  d.ground = GroundBehavior::None;
  d.maxParticles = 160;
  return d;
}

ParticleEffectDesc recolourBlood(ParticleEffectDesc desc, glm::vec3 hi, glm::vec3 lo)
{
  desc.colorOverLife = Gradient::twoStop(hi, lo);
  return desc;
}

void registerBloodEffects(ParticleEngine& particles, const std::string& prefix)
{
  particles.registerEffect(prefix + "_mist", bloodMistEffect());
  particles.registerEffect(prefix + "_spray", bloodSprayEffect());
  particles.registerEffect(prefix + "_gobs", bloodGobsEffect());
  particles.registerEffect(prefix + "_drip", bloodDripEffect());
}

void registerBloodEffects(ParticleEngine& particles,
                          const std::string& prefix,
                          glm::vec3 hi,
                          glm::vec3 lo)
{
  particles.registerEffect(prefix + "_mist",
                           recolourBlood(bloodMistEffect(), hi, lo));
  particles.registerEffect(prefix + "_spray",
                           recolourBlood(bloodSprayEffect(), hi, lo));
  particles.registerEffect(prefix + "_gobs",
                           recolourBlood(bloodGobsEffect(), hi, lo));
  particles.registerEffect(prefix + "_drip",
                           recolourBlood(bloodDripEffect(), hi, lo));
}

} // namespace sfs
