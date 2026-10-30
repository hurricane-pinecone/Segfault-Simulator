#pragma once

#include "engine/core/particles/decal.h"
#include "engine/runtime/rendering/modules/renderModule.h"
#include "engine/runtime/rendering/renderContext.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace sfs
{

class AssetStore;

/**
 * Marks left on a 2D game's surfaces (blood, scorches, paint): what particles
 * that stain stamp (ParticleModule::enableStains), or anything that calls
 * addDecal(). Each lies flat on the game's plane where it landed, placed by
 * the game's Space2D (see RenderSystem::setSpace2D), a tinted image (soft
 * round, or crisp for streaks) drawn see-through on the layer it's given. A
 * mark can fade away, run (a drip growing down), and be cut to the surface it
 * landed on.
 */
class DecalModule : public CommandModule<RenderContext, MeshDrawCommand>,
                    public IDecalSink
{
public:
  explicit DecalModule(AssetStore& assets);

  /** The RenderLayerComponent layer marks lie on (in front of layer 0's). */
  void setLayer(int layer) { m_layer = layer; }
  /** The most marks kept; the oldest go first. */
  void setMaxDecals(std::size_t count) { m_max = count; }
  /**
   * At most `perCell` lasting marks in each `cellSize` square of the plane,
   * so a spot hit again and again doesn't pile up marks; 0: no limit.
   */
  void setCoverage(float cellSize, int perCell);

  std::size_t count() const { return m_decals.size(); }

  void addDecal(const DecalSpawn& spawn) override;
  void update(double deltaTime) override;
  void computeCommands(const RenderContext& context) override;

private:
  struct Decal
  {
    glm::vec2 position{0.0f};
    float height = 0.0f;
    glm::vec2 size{1.0f};
    glm::vec2 anchor{0.5f}; // its image's point at `position` (y down)
    glm::vec2 growTo{0.0f}; // > 0: that size eases toward it
    float growRate = 0.0f;
    float rotation = 0.0f;
    glm::vec4 color{1.0f};
    std::string image;
    float lifetime = -1.0f; // < 0: lasting
    float age = 0.0f;
    bool clipped = false;
    glm::vec2 clipMin{0.0f};
    glm::vec2 clipMax{0.0f};
  };

  long long cellOf(glm::vec2 position) const;

  AssetStore& m_assets;
  int m_layer = 1;
  std::size_t m_max = 20000;
  float m_cellSize = 0.0f;
  int m_perCell = 0;
  std::vector<Decal> m_decals; // oldest first
  MeshHandle m_quad;
  std::unordered_map<std::string, MaterialHandle> m_materials;
};

} // namespace sfs
