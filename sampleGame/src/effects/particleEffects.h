#pragma once

#include "engine/particles/particleEffect.h"
#include "glm/glm/ext/vector_float3.hpp"

// Sample-game particle effect presets. These are plain ParticleEffectDesc
// builders (the same struct a future Lua/JSON loader would fill), registered on
// the engine's ParticleSystem by name.

// Blood splatter: a one-shot burst of small dark-red droplets that pop up and
// out, fall under gravity, and pool on the ground briefly. Uses the engine's
// "white_pixel" texture, so it needs no art -- per-particle colour does the work.
inline sfs::ParticleEffectDesc makeBloodEffect()
{
  sfs::ParticleEffectDesc d;

  d.texture = "white_pixel";
  d.blend = sfs::BlendMode::Alpha;
  d.space = sfs::SimulationSpace::World;

  d.shape = sfs::EmissionShape::Circle;
  d.shapeRadius = 0.08f;
  d.burstCount = 28;

  d.lifetime = sfs::FloatRange::of(0.45f, 1.05f);
  d.speed = sfs::FloatRange::of(1.5f, 4.2f);          // outward spray (tiles/s)
  d.launchHeightSpeed = sfs::FloatRange::of(2.2f, 4.8f); // upward pop
  d.startHeight = sfs::FloatRange::of(0.1f, 0.35f);
  d.size = sfs::FloatRange::of(0.05f, 0.13f);          // droplet size (tiles)
  d.angularVelocity = sfs::FloatRange::of(-6.0f, 6.0f);
  d.directionSpread = 6.28318530718f;                  // omnidirectional

  d.gravityZ = -15.0f; // falls quickly
  d.drag = 1.6f;       // planar spread slows

  d.colorOverLife =
      sfs::Gradient::twoStop(glm::vec3{0.62f, 0.02f, 0.02f},
                             glm::vec3{0.24f, 0.0f, 0.0f});

  d.alphaOverLife = sfs::Curve{}.add(0.0f, 1.0f).add(0.7f, 1.0f).add(1.0f, 0.0f);
  d.sizeOverLife =
      sfs::Curve{}.add(0.0f, 0.7f).add(0.2f, 1.0f).add(1.0f, 0.85f);

  d.ground = sfs::GroundBehavior::Stick; // pool on the floor
  d.stickDuration = 0.7f;

  d.maxParticles = 256;
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

  d.lifetime = sfs::FloatRange::of(0.8f, 1.7f);
  d.speed = sfs::FloatRange::of(0.1f, 0.5f);
  d.launchHeightSpeed = sfs::FloatRange::of(0.6f, 1.3f); // drift upward
  d.startHeight = sfs::FloatRange::of(0.15f, 0.45f);
  d.size = sfs::FloatRange::of(0.04f, 0.09f);

  d.gravityZ = 0.4f; // gentle rise
  d.drag = 0.6f;

  d.colorOverLife =
      sfs::Gradient::twoStop(glm::vec3{1.0f, 0.6f, 0.2f},
                             glm::vec3{0.85f, 0.12f, 0.0f});

  d.alphaOverLife =
      sfs::Curve{}.add(0.0f, 0.0f).add(0.15f, 1.0f).add(1.0f, 0.0f);

  d.ground = sfs::GroundBehavior::None;
  d.maxParticles = 160;
  return d;
}
