#pragma once

#include "engine/assetStore/assetStore.h"
#include "engine/ecs/ecs.h" // IWYU pragma: keep
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

class IsometricShadowSystem
    : public System,
      public RenderProvider<IsometricRenderContext, TerrainShadowBatchCommand>
{
public:
  IsometricShadowSystem();
  IsometricShadowSystem(IsometricShadowSettings settings,
                        const AssetStore* assetStore = nullptr)
      : m_shadowSettings(settings) {};

  void computeCommands(const IsometricRenderContext& context) override;

  void markTerrainDirty();

private:
  void computeTerrainShadows(const IsometricRenderContext& context);

  void setTerrainShadowMaxLength(float length);
  void setTerrainShadowAlpha(float alpha);

  void constructWallShadowFace(const IsometricRenderContext& renderContext,
                               std::vector<TerrainShadowCommand>& outCommands,
                               const glm::ivec2& tile,
                               int elevation,
                               int side,
                               const glm::vec2 shadowWorldPoints[4],
                               float incomingElevation,
                               float alpha);

  void constructTerrainEdgeShadowProjectedClipped(
      const IsometricRenderContext& renderContext,
      std::vector<TerrainShadowCommand>& outCommands,
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

  TerrainShadowCommand
  buildTerrainShadowCommand(const glm::vec2 screenPoints[4],
                            float alpha,
                            float sortKey);

  std::vector<TerrainShadowBatchCommand> batchTerrainShadowCommands(
      const std::vector<TerrainShadowCommand>& items) const;

  void emitTileShadow(const IsometricRenderContext& context,
                      std::vector<TerrainShadowCommand>& outCommands,
                      const glm::ivec2& tile,
                      int elevation,
                      const ClippedPolygon& poly,
                      float alpha);

protected:
  void create() override;

private:
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

  struct RenderOrderKey
  {
    RenderPass pass;
    float depth;
    int subpass;

    bool operator<(const RenderOrderKey& other) const
    {
      if (pass != other.pass)
        return pass < other.pass;

      if (depth != other.depth)
        return depth < other.depth;

      return subpass < other.subpass;
    }
  };

  TerrainShadowCache m_cache;
  IsometricShadowSettings m_shadowSettings;
};
} // namespace sfs
