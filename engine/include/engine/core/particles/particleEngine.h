#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/registry.h" // IWYU pragma: keep -- registry->view<T...>()
#include "engine/core/particles/decal.h"
#include "engine/core/particles/iParticleCollisionSource.h"
#include "engine/core/particles/particle.h"
#include "engine/core/particles/particleBatch.h"
#include "engine/core/particles/particleEffect.h"
#include "engine/core/rendering/iProjection.h"
#include "engine/core/types/blendMode.h"
#include "glm/glm/common.hpp"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_int2.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sfs
{

// Per-spawn physics override, layered on top of the effect's own emission. Lets
// a caller inject the momentum of whatever produced the burst (a bullet hit, an
// explosion, a footstep) so the same effect can spray in any direction.
struct ParticleSpawnParams
{
  // Added to every emitted particle's initial velocity (world tiles/sec, and
  // height/sec for Z). This is the inherited/impact momentum.
  glm::vec2 velocity{0.0f, 0.0f};
  float velocityZ = 0.0f;

  // When true and `velocity` is non-zero, the emission spread/cone is centred
  // on the velocity's direction instead of +X, so the spray fans along the
  // impact.
  bool aimAlongVelocity = true;
};

// A drawable batch of particles: the projected geometry plus the render
// attributes a renderer needs to pack it into a draw command. Render-command and
// render-pass types stay out of here so the engine has no render dependency.
struct ParticleRenderBatch
{
  ParticleBatch geometry;
  const std::string* textureId = nullptr;
  BlendMode blend = BlendMode::Alpha;
  bool screenSpace = false; // screen overlay (no terrain occlusion) vs world
  float depth = 0.0f;       // world sort-key (0 for screen-space)
};

// Render-context-free particle engine: simulates particles in simulate() and
// projects live ones into ParticleRenderBatches through an IProjection in
// buildBatches(). It depends only on the world-to-screen projection interface,
// so the same engine drives the isometric path and a flat-2D one; a render
// module turns the batches into draw commands.
//
// Effects are registered by name as POD ParticleEffectDesc. Two spawn paths:
//   - spawnBurst(name, worldPos, elevation): fire-and-forget one-shots.
//   - ParticleEmitterComponent on an entity: continuous effect that follows it.
class ParticleEngine
{
public:
  // The registry the entity-emitter sync reads from (set by the owning module).
  void setRegistry(Registry* r) { registry = r; }

  // Register (or replace) an effect under a name. The name keys spawnBurst()
  // and ParticleEmitterComponent::effect.
  void registerEffect(const std::string& name, const ParticleEffectDesc& desc);
  bool hasEffect(const std::string& name) const;

  // The registered effect's description, or nullptr if unknown. Lets a live
  // editor read a desc, tweak fields, and re-register it (registerEffect
  // replaces by name) to reconfigure the running game.
  const ParticleEffectDesc* effect(const std::string& name) const;

  // Names of every registered effect, for discovery (e.g. a Lua console listing
  // what can be spawned/configured).
  std::vector<std::string> effectNames() const;

  // Fire a one-shot burst at a world tile position sitting on the given ground
  // elevation level. The optional spawn params inject impact
  // momentum/direction. Returns false if the effect name is unknown.
  bool spawnBurst(const std::string& effect,
                  glm::vec2 worldPos,
                  float elevation = 0.0f,
                  const ParticleSpawnParams& spawn = {});

  // Fire a one-shot burst of a SimulationSpace::Screen effect at a screen
  // pixel.
  bool spawnScreenBurst(const std::string& effect,
                        glm::vec2 screenPos,
                        const ParticleSpawnParams& spawn = {});

  // Optional decal sink. When set, particles whose effect has leavesDecal stamp
  // a persistent mark on the surface they hit.
  void setDecalSink(IDecalSink* sink) { m_decalSink = sink; }

  // The surface particles collide with and stick to (terrain heightfield or
  // scene colliders). When set, world particles with a GroundBehavior stick to
  // it and leave decals via the sink; it also answers ground-elevation for
  // terrain-aware spawns. Without one, particles fall to their spawn plane.
  void setCollisionSource(const IParticleCollisionSource* source)
  {
    m_collisionSource = source;
  }

  int liveParticleCount() const;

  // Global ceiling on total live particles across every emitter + burst. Once
  // reached, new emissions are dropped (graceful) so stacked effects can't run
  // the particle count -- and the frame cost -- away.
  void setMaxParticles(int max) { m_maxParticles = max; }

  // Ground elevation at a world position, from the collision source (0 if none
  // is set). Lets a caller spawn a burst on the terrain without its own lookup.
  float groundElevationAt(glm::vec2 worldPos) const
  {
    return m_collisionSource ? m_collisionSource->groundElevation(worldPos)
                             : 0.0f;
  }

  // Advance every emitter and burst.
  void simulate(double deltaTime);

  // Project live particles into drawable batches through the projection.
  void buildBatches(const IProjection& projection,
                    std::vector<ParticleRenderBatch>& out);

private:
  Registry* registry = nullptr;

  using EffectId = int;

  struct EmitterInstance
  {
    EffectId effect = -1;
    bool screenSpace = false;

    // Origin: world tiles + ground elevation level, or screen pixels if
    // screenSpace.
    glm::vec2 origin{0.0f, 0.0f};
    float groundLevel = 0.0f;
    float spawnHeightBias = 0.0f; // emitter height offset added to startHeight

    // Impact momentum added to each emitted particle, and the spread centre
    // (radians) the emission fans around (0 = +X). Set from
    // ParticleSpawnParams.
    glm::vec2 baseVelocity{0.0f, 0.0f};
    float baseVelocityZ = 0.0f;
    float aimAngle = 0.0f;

    // Continuous component emitters track an entity and re-emit; a finished
    // entity (removed/disabled) stops emitting and drains.
    bool emitting = true;

    // Emission bookkeeping.
    float emitAccumulator = 0.0f;
    float burstTimer = 0.0f;
    int pendingBurst = 0;

    std::vector<Particle> particles;
    uint32_t rng = 0x9e3779b9u;
  };

  EffectId effectIdOf(const std::string& name) const;

  static void applySpawnParams(EmitterInstance& inst,
                               const ParticleSpawnParams& spawn);
  void emitParticles(EmitterInstance& inst,
                     const ParticleEffectDesc& desc,
                     int count);
  void
  stepInstance(EmitterInstance& inst, const ParticleEffectDesc& desc, float dt);

  // Stamp the splatter for a collision: the shaper produces the marks, then the
  // hit's surface (ground/water vs wall) decides how they're placed.
  void emitDecal(EmitterInstance& inst,
                 const ParticleEffectDesc& desc,
                 const Particle& p,
                 const ParticleHit& hit);
  void syncComponentEmitters();
  void appendBatch(const EmitterInstance& inst,
                   const ParticleEffectDesc& desc,
                   const IProjection& projection,
                   std::vector<ParticleRenderBatch>& out);

  const IParticleCollisionSource* m_collisionSource = nullptr;
  IDecalSink* m_decalSink = nullptr;

  std::vector<ParticleEffectDesc> m_effects;
  std::unordered_map<std::string, EffectId> m_effectIds;

  // Continuous emitters bound to an entity (keyed by entity id) and transient
  // fire-and-forget bursts. Both drain their particles before being dropped.
  std::unordered_map<Entity::EntityId, EmitterInstance> m_entityEmitters;
  std::vector<EmitterInstance> m_bursts;

  // Global live-particle ceiling (see setMaxParticles).
  int m_maxParticles = 6000;

  uint32_t m_burstSeed = 0x6d2b79f5u;
};

} // namespace sfs
