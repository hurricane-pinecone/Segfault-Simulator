#pragma once

#include "engine/assetStore/assetStore.h"
#include "engine/ecs/system.h"
#include "engine/renderers/commands/shadowCommands.h"
#include "engine/renderers/isometricRenderContext.h"
#include "engine/renderers/renderProvider.h"
#include "engine/renderers/util/isometric/geometry.h"
#include "engine/utils/isometricLightingUtils.h"
#include <atomic>
#include <vector>

#ifdef __EMSCRIPTEN__
  #include <future>
#endif

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
                        const AssetStore* assetStore = nullptr,
                        const IsometricAmbientLighting* ambient = nullptr)
      : m_shadowSettings(settings), m_ambientLighting(ambient) {};

  void computeCommands(const IsometricRenderContext& context) override;
  void setAmbientLighting(const IsometricAmbientLighting* ambient);

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

  // Multi threaded
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
  const IsometricAmbientLighting* m_ambientLighting = nullptr;

#ifdef __EMSCRIPTEN__
  struct ShadowBuildResult
  {
    std::vector<TerrainShadowCommand> items;
    int edgesProcessed = 0;
  };

  std::vector<std::future<ShadowBuildResult>>
  startTerrainEdgeShadowJobs(const IsometricRenderContext& context,
                             const glm::vec2& shadowDir,
                             float sunHeight,
                             float alpha);

  std::vector<std::future<ShadowBuildResult>> m_shadowJobs;
  bool m_shadowBuildInProgress = false;
#endif
};
} // namespace sfs
