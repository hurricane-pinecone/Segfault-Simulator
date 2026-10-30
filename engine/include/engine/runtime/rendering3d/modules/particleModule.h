#pragma once

#include "engine/core/particles/particleEngine.h"
#include "engine/runtime/rendering/modules/renderModule.h"
#include "engine/runtime/rendering3d/renderContext3D.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace sfs
{

class AssetStore;
class IDecalSink;
class IParticleCollisionSource;
class ITerrainSurfaceSource;

/**
 * Particle effects (see ParticleEngine): simulated in the game's own space
 * (a position on its plane and a height), drawn as quads facing the camera,
 * their texture times their colour, unlit, see-through or glowing as each
 * effect blends. A 2D game's are placed by its Space2D (see RenderSystem3D::
 * setSpace2D), as its sprites; in a 3D world a particle's plane is the ground
 * (XY) and its height is up (Z).
 */
class ParticleModule : public CommandModule<RenderContext3D, MeshDrawCommand>,
                       public ParticleEngine
{
public:
  explicit ParticleModule(AssetStore& assets);
  ~ParticleModule() override;

  /**
   * Let particles land and stain: they hit the scene's colliders (or, given
   * one, the terrain's surface) and stamp marks through `decals` (e.g. a
   * DecalModule).
   */
  void enableStains(IDecalSink& decals,
                    const ITerrainSurfaceSource* terrain = nullptr);

  void init(const ModuleInit& init) override;
  void update(double deltaTime) override { simulate(deltaTime); }
  void computeCommands(const RenderContext3D& context) override;
  std::vector<ModuleSetting> settings(const RenderContext3D&) override;

private:
  AssetStore& m_assets;
  Registry* m_registry = nullptr;
  std::unique_ptr<IParticleCollisionSource> m_collision;
  MeshHandle m_quad;
  std::unordered_map<std::string, MaterialHandle> m_materials;
};

} // namespace sfs
