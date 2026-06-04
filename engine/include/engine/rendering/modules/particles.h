#pragma once

#include "engine/ecs/entity.h"
#include "engine/ecs/registry.h" // IWYU pragma: keep -- registry->view<T...>()
#include "engine/particles/decal.h"
#include "engine/particles/particle.h"
#include "engine/particles/particleEffect.h"
#include "engine/rendering/commands/commands.h"
#include "engine/rendering/iProjection.h"
#include "engine/rendering/iTerrainSurfaceSource.h"
#include "engine/rendering/modules/renderModule.h"
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

// Context-free particle engine: simulates particles in simulate() and projects
// live ones into batched draw commands through an IProjection in
// buildCommands(). It depends only on the world-to-screen projection interface,
// not on any render context, so the same engine drives the isometric path and a
// flat-2D one. The Particles<TContext> module below wraps it as a render
// module.
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

  // Optional terrain awareness. When set, world particles whose effect has a
  // GroundBehavior collide with the real terrain (ground/walls/water) instead
  // of their spawn plane. Required for decals to land on the right surface.
  void setTerrainSource(const ITerrainSurfaceSource* source)
  {
    m_terrainSource = source;
  }

  // Optional decal sink. When set, particles whose effect has leavesDecal stamp
  // a persistent mark on the surface they hit.
  void setDecalSink(IDecalSink* sink) { m_decalSink = sink; }

  int liveParticleCount() const;

  // Advance every emitter and burst.
  void simulate(double deltaTime);

  // Project live particles into batched draw commands through the projection.
  void buildCommands(const IProjection& projection,
                     std::vector<ParticleBatchCommand>& out);

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

  // Stamp a decal for a particle that just collided with `surface` at `hitPos`/
  // `hitElev` (tile = the tile it hit, for the water fade-rate lookup).
  void emitDecal(EmitterInstance& inst,
                 const ParticleEffectDesc& desc,
                 const Particle& p,
                 glm::vec2 hitPos,
                 float hitElev,
                 DecalSurface surface,
                 uint8_t wallSide,
                 float wallBottom,
                 float wallTop,
                 glm::ivec2 tile);
  void syncComponentEmitters();
  void appendBatch(const EmitterInstance& inst,
                   const ParticleEffectDesc& desc,
                   const IProjection& projection,
                   std::vector<ParticleBatchCommand>& out);

  const ITerrainSurfaceSource* m_terrainSource = nullptr;
  IDecalSink* m_decalSink = nullptr;

  std::vector<ParticleEffectDesc> m_effects;
  std::unordered_map<std::string, EffectId> m_effectIds;

  // Continuous emitters bound to an entity (keyed by entity id) and transient
  // fire-and-forget bursts. Both drain their particles before being dropped.
  std::unordered_map<Entity::EntityId, EmitterInstance> m_entityEmitters;
  std::vector<EmitterInstance> m_bursts;

  uint32_t m_burstSeed = 0x6d2b79f5u;
};

/**
 * Render module wrapping a ParticleEngine for a given render context. The
 * module is the only context-typed piece -- it pulls the projection off the
 * context and hands it to the engine -- so the particle simulation and its
 * spawn/effect API stay context-free and reusable across render systems.
 *
 * Configure it through the inherited ParticleEngine API (registerEffect,
 * spawnBurst, setDecalSink, setTerrainSource).
 */
template <typename TContext>
class Particles : public CommandModule<TContext, ParticleBatchCommand>,
                  public ParticleEngine
{
public:
  void init(const ModuleInit& m) override { setRegistry(m.registry); }

  void update(double deltaTime) override { simulate(deltaTime); }

  void computeCommands(const TContext& context) override
  {
    this->flush();

    if (context.projection)
      buildCommands(*context.projection, this->m_commands);
  }

  std::vector<ModuleSetting> settings(const TContext&) override
  {
    return {
        settings::text("Live particles",
                       [this] { return std::to_string(liveParticleCount()); }),
    };
  }
};

} // namespace sfs
