#pragma once

#include "engine/core/particles/particleEngine.h"
#include "engine/runtime/rendering/commands/commands.h"
#include "engine/runtime/rendering/modules/renderModule.h"
#include "engine/runtime/rendering/renderPass.h"
#include <string>
#include <vector>

namespace sfs
{

/**
 * Render module wrapping a ParticleEngine for a given render context. The module
 * is the only context-typed piece -- it pulls the projection off the context,
 * has the engine project its live particles into ParticleRenderBatches, and
 * packs those into ParticleBatchCommands -- so the simulation and its
 * spawn/effect API stay render-context-free and reusable across render systems.
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

    if (!context.projection)
      return;

    std::vector<ParticleRenderBatch> batches;
    buildBatches(*context.projection, batches);

    for (ParticleRenderBatch& batch : batches)
    {
      ParticleBatchCommand cmd;
      cmd.quad = std::move(batch.geometry);
      cmd.textureId = batch.textureId;
      cmd.blend = batch.blend;
      cmd.order.pass = batch.screenSpace ? RenderPass::UI : RenderPass::Particles;
      cmd.order.subpass = 0;
      cmd.order.depth = batch.depth;
      this->m_commands.push_back(std::move(cmd));
    }
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
