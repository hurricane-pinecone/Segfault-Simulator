#pragma once

#include "engine/runtime/rendering/modules/renderModule.h"
#include "engine/runtime/rendering3d/renderContext3D.h"
#include "engine/runtime/rendering3d/space2D.h"

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace sfs
{

class AssetStore;

/**
 * Draws SpriteComponent entities: each a quad mesh showing its sprite sheet
 * frame (MeshInstance::uvRect), with the sheet as its material, lit and
 * shadowed like any mesh. Sheets with soft edges draw see-through; the rest
 * are cut out at half alpha.
 *
 * Flat sprites belong to a 2D game: placed by its Space2D (flat or
 * isometric; see RenderSystem3D::setSpace2D), lying in the XY plane, each
 * RenderLayerComponent layer a unit nearer its camera. Upright sprites stand
 * in a 3D world, turned to face the camera.
 */
class SpriteModule : public CommandModule<RenderContext3D, MeshDrawCommand>
{
public:
  enum class Facing
  {
    Flat,
    Upright,
  };

  explicit SpriteModule(AssetStore& assets, Facing facing = Facing::Flat);

  /** Upright sprites' pixels per world unit (a 2D game's: its Space2D's). */
  void setPixelsPerUnit(float pixels) { m_pixelsPerUnit = pixels; }

  void setFacing(Facing facing) { m_facing = facing; }
  Facing facing() const { return m_facing; }

  void init(const ModuleInit& init) override { m_registry = init.registry; }
  void computeCommands(const RenderContext3D& context) override;

private:
  // A sheet (and its normal map) as a material: made once.
  MaterialHandle material(MeshLibrary& meshes,
                          const std::string& sheet,
                          const std::string& normalSheet);

  AssetStore& m_assets;
  Registry* m_registry = nullptr;
  Facing m_facing;
  float m_pixelsPerUnit = 1.0f;
  MeshHandle m_quad;
  std::unordered_map<std::string, MaterialHandle> m_materials;
  std::unordered_set<uint32_t> m_softMaterials; // drawn see-through, together
};

} // namespace sfs
