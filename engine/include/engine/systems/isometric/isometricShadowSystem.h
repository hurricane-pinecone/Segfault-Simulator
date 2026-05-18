#pragma once

#include "engine/ecs/system.h"
#include "engine/renderers/isometricRenderContext.h"
#include "engine/renderers/isometricRenderItem.h"
#include "engine/renderers/isometricRenderQueue.h"

namespace sfs
{

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

class IsometricLightingSystem;

class IsometricShadowSystem : public System
{
public:
  IsometricShadowSystem();

  void submitSpriteShadow(const IsometricRenderContext& renderContext,
                          const IsometricRenderItem& caster,
                          const glm::vec2& spriteWorldSample,
                          int elevationLevel,
                          const IsometricLightingSystem& lightingSystem,
                          IsometricRenderQueue& queue);

  void submitTerrainEdgeShadows(const IsometricRenderContext& context,
                                const IsometricLightingSystem& lightingSystem,
                                IsometricRenderQueue& queue);

  void markTerrainDirty();

private:
  void submitTerrainShadow(std::vector<IsometricRenderItem>& outItems,
                           const glm::vec2 screenPoints[4],
                           SDL_Color tint,
                           float sortKey);

  void submitTileShadowPolygonAt(const IsometricRenderContext& renderContext,
                                 std::vector<IsometricRenderItem>& outItems,
                                 const glm::ivec2& tile,
                                 int elevation,
                                 const glm::vec2 worldPoints[4],
                                 float alpha);

  void submitWallShadowFace(const IsometricRenderContext& renderContext,
                            std::vector<IsometricRenderItem>& outItems,
                            const glm::ivec2& tile,
                            int elevation,
                            int side,
                            const glm::vec2 shadowWorldPoints[4],
                            float incomingElevation,
                            float normalizedDistance,
                            float alpha);

  void submitTerrainEdgeShadowProjectedClipped(
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

  TerrainShadowCache m_cache;
};

} // namespace sfs
