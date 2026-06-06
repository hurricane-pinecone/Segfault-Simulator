#pragma once

#include "engine/core/particles/particleEngine.h"
#include "engine/core/rendering/renderPass.h"
#include "engine/runtime/particles/colliderParticleCollision.h"
#include "engine/runtime/particles/terrainParticleCollision.h"
#include "engine/runtime/rendering/commands/commands.h"
#include "engine/runtime/rendering/modules/renderModule.h"
#include "engine/runtime/rendering/modules/renderModuleHost.h"
#include <memory>
#include <string>
#include <vector>

namespace sfs
{

/**
 * Render module wrapping a ParticleEngine for a given render context. The
 * module is the only context-typed piece -- it pulls the projection off the
 * context, has the engine project its live particles into
 * ParticleRenderBatches, and packs those into ParticleBatchCommands -- so the
 * simulation and its spawn/effect API stay render-context-free and reusable
 * across render systems.
 *
 * Register effects through the inherited ParticleEngine API; call
 * enableStains() once to make decal-leaving effects stick to the host's surface
 * (no manual sink or collision wiring).
 */
template <typename TContext>
class Particles : public CommandModule<TContext, ParticleBatchCommand>,
                  public ParticleEngine
{
public:
  void init(const ModuleInit& m) override
  {
    m_registry = m.registry;
    setRegistry(m.registry);
  }

  void setHost(RenderModuleHost<TContext>& host) override { m_host = &host; }

  // Turn on persistent splatter: effects with leavesDecal now stick to a
  // surface and stamp a mark on it, using the host's decal sink. Pass a terrain
  // heightfield to stick to it (iso); pass nothing to stick to the scene's
  // colliders (flat). One call replaces wiring a sink + collision source by
  // hand.
  void enableStains(const ITerrainSurfaceSource* terrain = nullptr)
  {
    if (m_host)
      setDecalSink(m_host->decalSink());

    if (terrain)
      m_collision = std::make_unique<TerrainParticleCollision>(terrain);
    else
      m_collision = std::make_unique<ColliderParticleCollision>(m_registry);
    setCollisionSource(m_collision.get());
  }

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
      cmd.order.pass =
          batch.screenSpace ? RenderPass::UI : RenderPass::Particles;
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

private:
  RenderModuleHost<TContext>* m_host = nullptr;
  Registry* m_registry = nullptr;
  std::unique_ptr<IParticleCollisionSource> m_collision;
};

} // namespace sfs
