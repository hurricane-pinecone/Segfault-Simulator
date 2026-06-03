#pragma once

#include "engine/ecs/entity.h"
#include "engine/ecs/system.h"
#include "engine/particles/particle.h"
#include "engine/particles/particleEffect.h"
#include "engine/rendering/commands/commands.h"
#include "engine/rendering/isometricRenderContext.h"
#include "engine/rendering/renderProvider.h"
#include "glm/glm/ext/vector_float2.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sfs
{

// Engine particle system. Simulates particles in update() and emits batched draw
// commands in computeCommands(), pulled into the isometric render queue by
// IsometricRenderSystem (the same RenderProvider seam used by shadows/water), so
// world particles share the frame's depth normalization and occlude correctly.
//
// Effects are registered by name as POD ParticleEffectDesc. Two spawn paths:
//   - spawnBurst(name, worldPos, elevation): fire-and-forget one-shots.
//   - ParticleEmitterComponent on an entity: continuous effect that follows it.
class ParticleSystem
    : public System,
      public RenderProvider<IsometricRenderContext, ParticleBatchCommand>
{
public:
  // Register (or replace) an effect under a name. The name keys spawnBurst() and
  // ParticleEmitterComponent::effect.
  void registerEffect(const std::string& name, const ParticleEffectDesc& desc);
  bool hasEffect(const std::string& name) const;

  // Fire a one-shot burst at a world tile position sitting on the given ground
  // elevation level. Returns false if the effect name is unknown.
  bool
  spawnBurst(const std::string& effect, glm::vec2 worldPos, float elevation = 0.0f);

  // Fire a one-shot burst of a SimulationSpace::Screen effect at a screen pixel.
  bool spawnScreenBurst(const std::string& effect, glm::vec2 screenPos);

  int liveParticleCount() const;

  // RenderProvider: project live particles into batched draw commands.
  void computeCommands(const IsometricRenderContext& context) override;

protected:
  void create() override;
  void update(double deltaTime) override;

private:
  using EffectId = int;

  struct EmitterInstance
  {
    EffectId effect = -1;
    bool screenSpace = false;

    // Origin: world tiles + ground elevation level, or screen pixels if screenSpace.
    glm::vec2 origin{0.0f, 0.0f};
    float groundLevel = 0.0f;
    float spawnHeightBias = 0.0f; // emitter height offset added to startHeight

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

  void emit(EmitterInstance& inst, const ParticleEffectDesc& desc, int count);
  void
  stepInstance(EmitterInstance& inst, const ParticleEffectDesc& desc, float dt);
  void syncComponentEmitters();
  void appendBatch(const EmitterInstance& inst,
                   const ParticleEffectDesc& desc,
                   const IsometricRenderContext& context);

  std::vector<ParticleEffectDesc> m_effects;
  std::unordered_map<std::string, EffectId> m_effectIds;

  // Continuous emitters bound to an entity (keyed by entity id) and transient
  // fire-and-forget bursts. Both drain their particles before being dropped.
  std::unordered_map<Entity::EntityId, EmitterInstance> m_entityEmitters;
  std::vector<EmitterInstance> m_bursts;

  uint32_t m_burstSeed = 0x6d2b79f5u;
};

} // namespace sfs
