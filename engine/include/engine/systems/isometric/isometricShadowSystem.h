#pragma once

#include "engine/assetStore/assetStore.h"
#include "engine/ecs/system.h"
#include "engine/renderers/commands/shadowCommands.h"
#include "engine/renderers/isometricRenderContext.h"
#include "engine/renderers/renderProvider.h"
#include "engine/utils/isometricLightingUtils.h"
#include "glm/glm/geometric.hpp"
#include <atomic>
#include <map>
#include <vector>

#ifdef __EMSCRIPTEN__
  #include <future>
#endif

namespace sfs
{

extern std::atomic<uint64_t> gShadowPathChecks;
extern std::atomic<uint64_t> gShadowTilesTraversed;

struct TerrainShadowEdge
{
  enum class Side
  {
    West,
    East,
    North,
    South,
  };
  glm::vec2 a;
  glm::vec2 b;

  glm::ivec2 casterTile{0, 0};
  glm::ivec2 receiverTile{0, 0};

  int topElevation = 0;
  int bottomElevation = 0;

  Side side = Side::West;
};

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

  void
  constructTileShadowPolygonAt(const IsometricRenderContext& renderContext,
                               std::vector<TerrainShadowCommand>& outCommands,
                               const glm::ivec2& tile,
                               int elevation,
                               const glm::vec2 worldPoints[4],
                               float alpha);

  void constructWallShadowFace(const IsometricRenderContext& renderContext,
                               std::vector<TerrainShadowCommand>& outCommands,
                               const glm::ivec2& tile,
                               int elevation,
                               int side,
                               const glm::vec2 shadowWorldPoints[4],
                               float incomingElevation,
                               float normalizedDistance,
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

  bool tryConstructShadowOnTile(const IsometricRenderContext& context,
                                std::vector<TerrainShadowCommand>& outCommands,
                                const glm::ivec2& tile,
                                int requiredElevation,
                                const glm::vec2 worldPoints[4],
                                float alpha);

  float calculateTerrainShadowLength(const TerrainShadowEdge& edge,
                                     float sunHeight,
                                     float maxShadowLength);

  template <typename Visitor>
  void walkShadowEdgeRays(const TerrainShadowEdge& edge,
                          const glm::vec2& shadowDir,
                          float shadowLength,
                          Visitor&& visit);

  TerrainShadowCommand
  buildTerrainShadowCommand(const glm::vec2 screenPoints[4],
                            float alpha,
                            float sortKey);

  std::vector<TerrainShadowBatchCommand> batchTerrainShadowCommands(
      const std::vector<TerrainShadowCommand>& items) const;

private:
  struct TerrainShadowCache
  {
    std::vector<TerrainShadowEdge> edges;

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

  std::map<RenderOrderKey, std::vector<Quad>> shadowBatches;

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

template <typename Visitor>
void IsometricShadowSystem::walkShadowEdgeRays(const TerrainShadowEdge& edge,
                                               const glm::vec2& shadowDir,
                                               float shadowLength,
                                               Visitor&& visit)
{
  const float edgeLength = glm::length(edge.b - edge.a);

  constexpr float RaySpacing = 0.25;

  const int samples =
      std::max(2, static_cast<int>(std::ceil(edgeLength / RaySpacing)));

  for (int i = 0; i <= samples; i++)
  {
    const float t = static_cast<float>(i) / static_cast<float>(samples);
    const glm::vec2 p = glm::mix(edge.a, edge.b, t);

    walkGridDDA(p + shadowDir * 0.02f,
                shadowDir,
                shadowLength,
                [&](const glm::ivec2& tile, float)
                { return visit(tile, false); });
  }
}

} // namespace sfs
