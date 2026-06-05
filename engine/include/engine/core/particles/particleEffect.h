#pragma once

#include "engine/core/particles/particleCurves.h"
#include "engine/core/types/blendMode.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include <cstdint>
#include <string>

namespace sfs
{

// Where a particle's spawn position is sampled from, relative to the emitter.
enum class EmissionShape : uint8_t
{
  Point,  // all at the origin
  Circle, // random point inside a disk of shapeRadius
  Ring,   // on the circle of shapeRadius
  Box,    // random point inside boxExtents
  Cone,   // origin; coneAngleDegrees narrows the launch direction spread
};

// World = simulates in tile/elevation space and is occluded by terrain.
// Screen = simulates in screen pixels and draws as a flat overlay (no occlusion).
enum class SimulationSpace : uint8_t
{
  World,
  Screen,
};

// What happens when a world particle's height reaches the ground (height <= 0).
enum class GroundBehavior : uint8_t
{
  None, // ignore the ground plane (height just keeps going)
  Die,  // kill the particle on contact
  Stick // pin it to the ground and let it linger for stickDuration
};

// Inclusive [min,max] range; a value is drawn uniformly between them per particle.
struct FloatRange
{
  float min = 0.0f;
  float max = 0.0f;

  static FloatRange of(float v) { return {v, v}; }
  static FloatRange of(float a, float b) { return {a, b}; }
};

// The persistent mark a particle leaves where it lands (see ParticleEffectDesc::
// leavesDecal). Consumed by the Decals module via an IDecalSink.
struct DecalSpec
{
  std::string texture = "white_pixel"; // resolved by the renderer
  FloatRange size{0.1f, 0.22f};        // mark size in tiles
  bool useParticleColor = true;        // tint from the particle's current colour
  glm::vec3 color{0.35f, 0.0f, 0.0f};  // used when !useParticleColor
  float alpha = 0.85f;
};

// The authoring description of a particle effect. Plain data with fixed-capacity
// members (Curve/Gradient) and no std::function, so a Lua/JSON loader can build
// one later and hand it to Particles::registerEffect unchanged.
struct ParticleEffectDesc
{
  // --- emission shape (spawn position offset from the emitter origin) ---
  EmissionShape shape = EmissionShape::Point;
  float shapeRadius = 0.0f;          // Circle/Ring base radius (world tiles)
  float coneAngleDegrees = 30.0f;    // Cone launch half-angle
  glm::vec2 boxExtents{0.0f, 0.0f};  // Box half-extents (world tiles)

  // --- emission rate / bursts ---
  float emitRate = 0.0f;      // continuous particles/sec (component emitters)
  int burstCount = 0;         // particles per burst (spawnBurst / repeating)
  float burstInterval = 0.0f; // >0 = an emitter re-bursts every N seconds

  // --- per-particle initial state (sampled per particle) ---
  FloatRange lifetime{1.0f, 1.0f};
  FloatRange speed{0.0f, 0.0f};             // planar launch speed (tiles/sec)
  FloatRange launchHeightSpeed{0.0f, 0.0f}; // vertical launch speed (height/sec)
  FloatRange startHeight{0.0f, 0.0f};       // initial height above ground
  FloatRange size{1.0f, 1.0f};              // base size in tiles (x sizeOverLife)
  FloatRange rotation{0.0f, 0.0f};          // initial rotation (radians)
  FloatRange angularVelocity{0.0f, 0.0f};   // spin (radians/sec)

  // Planar launch direction is a random angle within this full spread (radians),
  // centred on +X. Default = full circle (omnidirectional spray). Cone overrides
  // this with coneAngleDegrees.
  float directionSpread = 6.28318530718f;

  // --- forces (world units) ---
  glm::vec2 gravity{0.0f, 0.0f}; // planar acceleration (tiles/sec^2)
  float gravityZ = 0.0f;         // vertical acceleration; negative = falls
  float drag = 0.0f;             // linear velocity damping per second (0 = none)

  // --- appearance over normalized life (age / lifetime) ---
  Curve sizeOverLife = Curve::constant(1.0f);
  Gradient colorOverLife = Gradient::constant(glm::vec3{1.0f});
  Curve alphaOverLife = Curve::constant(1.0f);

  // --- texture / sprite-sheet animation ---
  std::string texture;        // texture id resolved by the renderer
  int frameCols = 1;          // sprite-sheet columns
  int frameRows = 1;          // sprite-sheet rows
  float frameFps = 0.0f;      // >0 animates frames over time
  bool frameOverLife = false; // true maps normalized age across all frames

  // --- rendering / simulation ---
  BlendMode blend = BlendMode::Alpha;
  SimulationSpace space = SimulationSpace::World;
  GroundBehavior ground = GroundBehavior::None;
  float stickDuration = 0.0f; // Stick: extra seconds a grounded particle lingers

  // --- persistent decals ---
  // When true (and a decal sink + terrain source are wired), a particle leaves a
  // permanent mark on the surface it collides with. Ignored for Screen effects.
  bool leavesDecal = false;
  DecalSpec decal;

  // --- budget ---
  int maxParticles = 512; // capacity per live occurrence of this effect
};

} // namespace sfs
