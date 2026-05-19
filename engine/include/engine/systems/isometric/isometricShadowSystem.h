#pragma once

#include "engine/ecs/system.h"
#include "engine/renderers/isometricRenderContext.h"
#include "engine/renderers/isometricRenderItem.h"
#include "engine/renderers/isometricRenderQueue.h"
#include "engine/utils/isometricLightingUtils.h"
#include "glm/glm/geometric.hpp"
#include <atomic>

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

class IsometricShadowSystem : public System
{
public:
  IsometricShadowSystem();
  IsometricShadowSystem(IsometricShadowSettings settings)
      : m_shadowSettings(settings) {};

  void submitTerrainShadows(const IsometricRenderContext& context,
                            const IsometricAmbientLighting& lightingSystem,
                            IsometricRenderQueue& queue);

  void submitSpriteShadow(
      const IsometricRenderContext& context,
      const IsometricRenderItem& caster,
      const glm::vec2& casterWorldSample,
      int casterElevation,
      const IsometricAmbientLighting* ambientLighting,
      const std::vector<IsometricPointLightSnapshot>* pointLights,
      IsometricRenderQueue& queue);

  void markTerrainDirty();

  void setTerrainShadowMaxLength(float length);
  void setSpriteShadowMaxLength(float length);
  void setTerrainShadowAlpha(float alpha);
  void setSpriteShadowAlpha(float alpha);

private:
  void constructTerrainShadow(std::vector<IsometricRenderItem>& outItems,
                              const glm::vec2 screenPoints[4],
                              SDL_Color tint,
                              float sortKey);

  void constructTileShadowPolygonAt(const IsometricRenderContext& renderContext,
                                    std::vector<IsometricRenderItem>& outItems,
                                    const glm::ivec2& tile,
                                    int elevation,
                                    const glm::vec2 worldPoints[4],
                                    float alpha);

  void constructWallShadowFace(const IsometricRenderContext& renderContext,
                               std::vector<IsometricRenderItem>& outItems,
                               const glm::ivec2& tile,
                               int elevation,
                               int side,
                               const glm::vec2 shadowWorldPoints[4],
                               float incomingElevation,
                               float normalizedDistance,
                               float alpha);

  void constructTerrainEdgeShadowProjectedClipped(
      const IsometricRenderContext& renderContext,
      std::vector<IsometricRenderItem>& outItems,
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

  void
  getWallEdge(const glm::ivec2& tile, int side, glm::vec2& a, glm::vec2& b);

  std::vector<TerrainShadowEdge>
  mergeTerrainShadowEdges(const std::vector<TerrainShadowEdge>& input) const;

  std::vector<TerrainShadowEdge>
  getTerrainShadowEdges(const IsometricRenderContext& context) const;

  static bool segmentIntersectsSegment(glm::vec2 p1,
                                       glm::vec2 p2,
                                       glm::vec2 q1,
                                       glm::vec2 q2);

  static bool pointInConvexQuad(glm::vec2 p, const glm::vec2 quad[4]);

  static bool projectShadowOntoWallEdge(const glm::vec2 shadowPoly[4],
                                        glm::vec2 wallA,
                                        glm::vec2 wallB,
                                        float& outMinT,
                                        float& outMaxT);

  static std::vector<glm::vec2>
  clipPolygonAgainstEdge(const std::vector<glm::vec2>& input,
                         float edge,
                         int axis,
                         bool keepGreater);

  static std::vector<glm::vec2>
  clipPolygonToTile(const glm::vec2 worldPoints[4], const glm::ivec2& tile);

  // Multi threaded
  std::vector<IsometricRenderItem>
  buildTerrainEdgeShadowItems(const IsometricRenderContext& context,
                              const glm::vec2& shadowDir,
                              float sunHeight,
                              float alpha);

  bool tryConstructShadowOnTile(const IsometricRenderContext& context,
                                std::vector<IsometricRenderItem>& outItems,
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

private:
  struct TerrainShadowCache
  {
    std::vector<TerrainShadowEdge> edges;
    std::vector<IsometricRenderItem> items;

    bool edgesDirty = true;
    bool itemsDirty = true;

    glm::vec3 sunDir{999.0f, 999.0f, 999.0f};
    glm::vec2 isoCameraPosition{999.0f, 999.0f};
    float zoom = -1.0f;
  };

  struct CachedTerrainShadowPolygon
  {
    glm::vec2 screenPoints[4];
    float sortKey = 0.0f;
  };

#ifdef __EMSCRIPTEN__
  struct ShadowBuildResult
  {
    std::vector<IsometricRenderItem> items;
    int edgesProcessed = 0;
  };

  std::vector<std::future<ShadowBuildResult>>
  startTerrainEdgeShadowJobs(const IsometricRenderContext& context,
                             const glm::vec2& shadowDir,
                             float sunHeight,
                             float alpha);

  std::vector<std::future<ShadowBuildResult>> m_shadowJobs;
  bool m_shadowBuildInProgress = false;

  glm::vec2 m_pendingShadowDir{};
  float m_pendingSunHeight = 0.0f;
  float m_pendingAlpha = 0.0f;
#endif

  TerrainShadowCache m_cache;
  IsometricShadowSettings m_shadowSettings;
};

template <typename Visitor>
void IsometricShadowSystem::walkShadowEdgeRays(const TerrainShadowEdge& edge,
                                               const glm::vec2& shadowDir,
                                               float shadowLength,
                                               Visitor&& visit)
{
  const float edgeLength = glm::length(edge.b - edge.a);

  constexpr float RaySpacing = 0.1f;

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
