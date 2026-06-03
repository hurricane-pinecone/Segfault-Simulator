#pragma once

#include "engine/ecs/registry.h" // IWYU pragma: keep
#include "engine/rendering/commands/shadowCommands.h"
#include "engine/rendering/isometricRenderContext.h"
#include "engine/rendering/renderProvider.h"
#include "engine/rendering/util/isometric/geometry.h"
#include "engine/utils/isometricLightingUtils.h"
#include <atomic>
#include <vector>

namespace sfs
{

extern std::atomic<uint64_t> gShadowPathChecks;
extern std::atomic<uint64_t> gShadowTilesTraversed;

/**
 * Builds projected sun-shadow quads for terrain edges, occluding them against
 * the render context's terrain elevation grid. A render-helper owned by the
 * isometric render system: it pulls tiles from a Registry view and emits batched
 * shadow commands through the RenderProvider interface.
 */
class IsometricTerrainShadowProvider
    : public RenderProvider<IsometricRenderContext, TerrainShadowBatchCommand>
{
public:
  IsometricTerrainShadowProvider() = default;
  explicit IsometricTerrainShadowProvider(IsometricShadowSettings settings)
      : m_shadowSettings(settings) {};

  /** Set the registry the terrain-tile view reads from. */
  void setRegistry(Registry* r) { registry = r; }

  void computeCommands(const IsometricRenderContext& context) override;

  /** Invalidate the cached edge geometry so it rebuilds next frame. */
  void markTerrainDirty();

  IsometricShadowSettings& shadowSettings() { return m_shadowSettings; }

private:
  void computeTerrainShadows(const IsometricRenderContext& context);

  void setTerrainShadowMaxLength(float length);
  void setTerrainShadowAlpha(float alpha);

  void constructTerrainEdgeShadowProjectedClipped(
      const IsometricRenderContext& renderContext,
      std::vector<Quad>& outQuads,
      const TerrainShadowEdge& edge,
      const glm::vec2& shadowDir,
      float sunHeight,
      float maxShadowLength,
      float alpha);

  bool terrainShadowPathBlocked(const IsometricRenderContext& renderContext,
                                const TerrainShadowEdge& edge,
                                const glm::ivec2& receiverTile,
                                const glm::vec2& shadowDir) const;

  bool shouldCastTerrainShadow(const TerrainShadowEdge& edge,
                               const glm::vec2& shadowDir);

  std::vector<TerrainShadowEdge>
  mergeTerrainShadowEdges(const std::vector<TerrainShadowEdge>& input) const;

  std::vector<TerrainShadowEdge>
  getTerrainShadowEdges(const IsometricRenderContext& context) const;

  void buildTerrainEdgeShadowItems(const IsometricRenderContext& context,
                                   const glm::vec2& shadowDir,
                                   float sunHeight,
                                   float alpha);

  float calculateTerrainShadowLength(const TerrainShadowEdge& edge,
                                     float sunHeight,
                                     float maxShadowLength);

  void emitTileShadow(const IsometricRenderContext& context,
                      std::vector<Quad>& outQuads,
                      const glm::ivec2& tile,
                      int elevation,
                      const ClippedPolygon& poly,
                      float alpha);

private:
  Registry* registry = nullptr;

  struct TerrainShadowCache
  {
    std::vector<TerrainShadowEdge> edges;
    TileBounds edgeTileBounds;

    bool edgesDirty = true;
    bool itemsDirty = true;

    float alpha = -1.0f;

    glm::vec3 sunDir{999.0f, 999.0f, 999.0f};
    glm::vec2 isoCameraPosition{999.0f, 999.0f};
    float zoom = -1.0f;
  };

  TerrainShadowCache m_cache;
  IsometricShadowSettings m_shadowSettings;

  // Persistent per-worker-range scratch buffers, reused across rebuilds so we
  // don't reallocate them every frame (clear() keeps capacity).
  std::vector<std::vector<Quad>> m_shadowRangeQuads;
};
} // namespace sfs
