#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/runtime/rendering/modules/renderModule.h"
#include "engine/runtime/rendering/renderContext.h"
#include "engine/runtime/rendering/space2D.h"

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace sfs
{

class AssetStore;

/**
 * Draws SpriteComponent entities: each a quad mesh showing its sprite sheet
 * frame (MeshInstance::uvRect), with the sheet (and its normal map) as its
 * material, lit like any mesh, tinted, and shadowing if it casts shadows.
 * Sheets with soft edges draw see-through; the rest are cut out at half alpha.
 *
 * Flat sprites belong to a 2D game: placed by its Space2D (flat or
 * isometric; see RenderSystem::setSpace2D), lying in the XY plane, each
 * RenderLayerComponent layer a unit nearer its camera. Upright sprites stand
 * in a 3D world, turned to face the camera. By default (Facing::Auto) they're
 * whichever the scene's style calls for.
 */
class SpriteModule : public CommandModule<RenderContext, MeshDrawCommand>
{
public:
  enum class Facing
  {
    Auto, // as the game's style: flat in a 2D game, upright in 3D
    Flat,
    Upright,
  };

  explicit SpriteModule(AssetStore& assets, Facing facing = Facing::Auto);

  /**
   * In 3D, the pixels per world unit of sprites whose image doesn't set its
   * own (TextureImportSettings::pixelsPerUnit); default 1. A 2D game's are its
   * Space2D's.
   */
  void setPixelsPerUnit(float pixels) { m_pixelsPerUnit = pixels; }

  void setFacing(Facing facing) { m_facing = facing; }
  Facing facing() const { return m_facing; }

  /**
   * Where each sprite was drawn last frame: its quad's model matrix (a unit
   * plane, -0.5..0.5), by entity id. For picking and outlines.
   */
  const std::unordered_map<Entity::EntityId, glm::mat4>& placed() const
  {
    return m_placed;
  }

  void init(const ModuleInit& init) override { m_registry = init.registry; }
  void computeCommands(const RenderContext& context) override;

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
  struct SheetMaterial
  {
    MaterialHandle handle;
    const TextureData* image = nullptr; // what it was made from
  };
  std::unordered_map<std::string, SheetMaterial> m_materials;
  std::unordered_set<uint32_t> m_softMaterials; // drawn see-through, together
  std::unordered_map<Entity::EntityId, glm::mat4> m_placed;
};

} // namespace sfs
