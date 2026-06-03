#pragma once

#include "engine/particles/particleEffect.h"
#include "engine/rendering/modules/particles.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"

// Sample-game particle effect presets. These are plain ParticleEffectDesc
// builders (the same struct a future Lua/JSON loader would fill), registered on
// the engine's Particles module by name.

// A shotgun gore blast is three layered effects fired at the same point and
// direction (see spawnGore): a bright fast MIST haze, the main directional
// SPRAY of droplets, and a few heavy GOBS that hang back and splat. All use the
// engine's "white_pixel" texture, so they need no art -- colour does the work.

// Layer 1: fine, bright, fast-fading additive haze right at the impact.
inline sfs::ParticleEffectDesc makeBloodMistEffect()
{
  sfs::ParticleEffectDesc d;

  d.texture = "blood_dot";
  d.blend = sfs::BlendMode::Additive; // bright pink haze
  d.space = sfs::SimulationSpace::World;

  d.shape = sfs::EmissionShape::Circle;
  d.shapeRadius = 0.05f;
  d.burstCount = 28;

  d.lifetime = sfs::FloatRange::of(0.12f, 0.34f); // gone almost immediately
  d.speed = sfs::FloatRange::of(2.0f, 7.0f);
  d.launchHeightSpeed = sfs::FloatRange::of(1.0f, 3.0f);
  d.startHeight = sfs::FloatRange::of(0.1f, 0.5f);
  d.size = sfs::FloatRange::of(0.04f, 0.11f);
  d.directionSpread = 1.7f; // a touch wider than the spray -- it's a cloud

  d.gravityZ = -3.0f; // barely falls
  d.drag = 4.5f;      // slows fast and hangs

  d.colorOverLife = sfs::Gradient::twoStop(
      glm::vec3{1.0f, 0.3f, 0.3f}, glm::vec3{0.4f, 0.04f, 0.04f});
  d.alphaOverLife =
      sfs::Curve{}.add(0.0f, 0.0f).add(0.1f, 0.7f).add(1.0f, 0.0f);
  d.sizeOverLife = sfs::Curve{}.add(0.0f, 0.6f).add(1.0f, 1.4f); // expands

  d.ground = sfs::GroundBehavior::None;
  d.maxParticles = 128;
  return d;
}

// Layer 2: the main directional droplet spray (slimmed-down particle count).
inline sfs::ParticleEffectDesc makeBloodSprayEffect()
{
  sfs::ParticleEffectDesc d;

  d.texture = "blood_dot";
  d.blend = sfs::BlendMode::Alpha;
  d.space = sfs::SimulationSpace::World;

  d.shape = sfs::EmissionShape::Circle;
  d.shapeRadius = 0.06f;
  d.burstCount = 44; // minimal -- the mist + gobs carry the rest of the read

  // Lifetime must exceed the airtime, or droplets die mid-arc and never land
  // (which starved the ground of stains vs walls). Max launch ~3 + the gore's
  // vertical kick gives ~0.5 s airtime, so 0.7 s min comfortably lands them.
  d.lifetime = sfs::FloatRange::of(0.7f, 1.2f);
  // Scatter kept below the spawn impulse so the bulk push sets the direction.
  d.speed = sfs::FloatRange::of(1.5f, 6.0f);
  d.launchHeightSpeed = sfs::FloatRange::of(1.0f, 3.0f);
  d.startHeight = sfs::FloatRange::of(0.1f, 0.4f);
  d.size = sfs::FloatRange::of(0.05f, 0.15f);
  d.angularVelocity = sfs::FloatRange::of(-9.0f, 9.0f);
  d.directionSpread = 1.2f; // ~70 deg forward beam

  d.gravityZ = -18.0f;
  d.drag = 2.6f; // punches out, then decelerates hard

  d.colorOverLife = sfs::Gradient::twoStop(
      glm::vec3{0.8f, 0.05f, 0.05f}, glm::vec3{0.2f, 0.0f, 0.0f});
  d.alphaOverLife =
      sfs::Curve{}.add(0.0f, 1.0f).add(0.78f, 1.0f).add(1.0f, 0.0f);
  d.sizeOverLife =
      sfs::Curve{}.add(0.0f, 0.55f).add(0.12f, 1.0f).add(1.0f, 0.8f);

  d.ground = sfs::GroundBehavior::Stick;
  d.stickDuration = 1.1f;

  // Each droplet leaves a small permanent stain where it lands.
  d.leavesDecal = true;
  d.decal.texture = "blood_dot";
  d.decal.size = sfs::FloatRange::of(0.07f, 0.16f);
  d.decal.useParticleColor = true;
  d.decal.alpha = 0.8f;

  d.maxParticles = 256;
  return d;
}

// Layer 3: a few big, heavy chunks. They get LESS of the impulse (see
// spawnGore) so they lag the spray, arc, and leave fat splats that linger.
inline sfs::ParticleEffectDesc makeBloodGobsEffect()
{
  sfs::ParticleEffectDesc d;

  d.texture = "blood_dot";
  d.blend = sfs::BlendMode::Alpha;
  d.space = sfs::SimulationSpace::World;

  d.shape = sfs::EmissionShape::Circle;
  d.shapeRadius = 0.05f;
  d.burstCount = 10; // just a handful of chunks

  d.lifetime = sfs::FloatRange::of(0.9f, 1.6f);          // linger; outlast the arc
  d.speed = sfs::FloatRange::of(1.0f, 3.0f);             // low internal scatter
  d.launchHeightSpeed = sfs::FloatRange::of(1.5f, 3.5f); // tossed up to arc
  d.startHeight = sfs::FloatRange::of(0.1f, 0.4f);
  d.size = sfs::FloatRange::of(0.14f, 0.3f); // fat gobs
  d.angularVelocity = sfs::FloatRange::of(-5.0f, 5.0f);
  d.directionSpread = 1.4f;

  d.gravityZ = -14.0f; // arc down
  d.drag = 1.2f;       // less drag -- they carry their arc

  d.colorOverLife = sfs::Gradient::twoStop(
      glm::vec3{0.5f, 0.0f, 0.0f}, glm::vec3{0.16f, 0.0f, 0.0f});
  d.alphaOverLife =
      sfs::Curve{}.add(0.0f, 1.0f).add(0.8f, 1.0f).add(1.0f, 0.0f);
  d.sizeOverLife = sfs::Curve{}.add(0.0f, 0.7f).add(0.2f, 1.0f).add(1.0f, 0.9f);

  d.ground = sfs::GroundBehavior::Stick;
  d.stickDuration = 1.6f; // big splats stay a while

  // Heavy gobs leave fat splats -- but the decal is smaller than the in-air gob
  // so it reads as a mark on the surface, not a floating blob.
  d.leavesDecal = true;
  d.decal.texture = "blood_dot";
  d.decal.size = sfs::FloatRange::of(0.12f, 0.22f);
  d.decal.useParticleColor = true;
  d.decal.alpha = 0.9f;

  d.maxParticles = 96;
  return d;
}

// Warm embers: a continuous additive drift, meant for an emitter component on a
// light source. Proves additive blend + the entity-bound emitter path.
inline sfs::ParticleEffectDesc makeEmberEffect()
{
  sfs::ParticleEffectDesc d;

  d.texture = "white_pixel";
  d.blend = sfs::BlendMode::Additive;
  d.space = sfs::SimulationSpace::World;

  d.shape = sfs::EmissionShape::Circle;
  d.shapeRadius = 0.12f;
  d.emitRate = 16.0f; // particles/second

  d.lifetime = sfs::FloatRange::of(0.8f, 1.9f);
  d.speed = sfs::FloatRange::of(0.1f, 0.5f);
  d.launchHeightSpeed = sfs::FloatRange::of(0.6f, 1.3f); // drift upward
  d.startHeight = sfs::FloatRange::of(0.15f, 0.45f);
  d.size = sfs::FloatRange::of(0.04f, 0.09f);

  d.gravityZ = 0.4f; // gentle rise
  d.drag = 0.6f;

  d.colorOverLife = sfs::Gradient::twoStop(
      glm::vec3{1.0f, 0.6f, 0.2f}, glm::vec3{0.85f, 0.12f, 0.0f});

  d.alphaOverLife =
      sfs::Curve{}.add(0.0f, 0.0f).add(0.15f, 1.0f).add(1.0f, 0.0f);

  d.ground = sfs::GroundBehavior::None;
  d.maxParticles = 160;
  return d;
}

// Layer 4: blood that just drops at the impact. Almost no outward velocity (and
// spawnGore gives it almost no impulse), so it pops up a little and falls back
// onto the spawn tile, reliably staining the spot the shot landed.
inline sfs::ParticleEffectDesc makeBloodDripEffect()
{
  sfs::ParticleEffectDesc d;

  d.texture = "blood_dot";
  d.blend = sfs::BlendMode::Alpha;
  d.space = sfs::SimulationSpace::World;

  d.shape = sfs::EmissionShape::Circle;
  d.shapeRadius = 0.1f;
  d.burstCount = 20;

  d.lifetime = sfs::FloatRange::of(0.3f, 0.8f);
  d.speed = sfs::FloatRange::of(0.1f, 1.0f); // barely spreads
  d.launchHeightSpeed =
      sfs::FloatRange::of(0.5f, 2.4f); // small pop straight up
  d.startHeight = sfs::FloatRange::of(0.05f, 0.25f);
  d.size = sfs::FloatRange::of(0.05f, 0.12f);
  d.angularVelocity = sfs::FloatRange::of(-4.0f, 4.0f);
  d.directionSpread = 6.28318530718f; // omnidirectional little plops

  d.gravityZ = -16.0f;
  d.drag = 3.2f; // kills the little outward speed fast -> stays put

  d.colorOverLife = sfs::Gradient::twoStop(
      glm::vec3{0.7f, 0.03f, 0.03f}, glm::vec3{0.2f, 0.0f, 0.0f});
  d.alphaOverLife =
      sfs::Curve{}.add(0.0f, 1.0f).add(0.8f, 1.0f).add(1.0f, 0.0f);

  d.ground = sfs::GroundBehavior::Stick;
  d.stickDuration = 1.0f;

  d.leavesDecal = true;
  d.decal.texture = "blood_dot";
  d.decal.size = sfs::FloatRange::of(0.06f, 0.13f);
  d.decal.useParticleColor = true;
  d.decal.alpha = 0.85f;

  d.maxParticles = 128;
  return d;
}

// Register the gore layers under the names spawnGore() fires.
inline void registerGoreEffects(sfs::Particles& particles)
{
  particles.registerEffect("blood_mist", makeBloodMistEffect());
  particles.registerEffect("blood_spray", makeBloodSprayEffect());
  particles.registerEffect("blood_gobs", makeBloodGobsEffect());
  particles.registerEffect("blood_drip", makeBloodDripEffect());
}

// Fire a full shotgun gore blast at a world tile. `direction` should be a unit
// vector (the shot's travel); `power` is the spray's impulse in tiles/sec. Each
// layer gets its own share of that impulse: the mist races slightly ahead, the
// spray takes the full push, and the heavy gobs get only a fraction so they lag
// behind and splat.
inline void spawnGore(sfs::Particles& particles,
                      glm::vec2 worldPos,
                      float elevation,
                      glm::vec2 direction,
                      float power)
{
  sfs::ParticleSpawnParams mist;
  mist.velocity = direction * (power * 1.15f);
  mist.velocityZ = 2.0f;

  sfs::ParticleSpawnParams spray;
  spray.velocity = direction * power;
  spray.velocityZ = 1.5f; // modest kick so droplets land within their lifetime

  sfs::ParticleSpawnParams gobs;
  gobs.velocity =
      direction * (power * 0.4f); // heavy chunks: much less velocity
  gobs.velocityZ = 2.5f;          // arc, but still lands within its lifetime

  sfs::ParticleSpawnParams drip;
  drip.velocity = direction * (power * 0.04f); // essentially drops in place
  drip.velocityZ = 1.5f;

  particles.spawnBurst("blood_mist", worldPos, elevation, mist);
  particles.spawnBurst("blood_spray", worldPos, elevation, spray);
  particles.spawnBurst("blood_gobs", worldPos, elevation, gobs);
  particles.spawnBurst("blood_drip", worldPos, elevation, drip);
}
