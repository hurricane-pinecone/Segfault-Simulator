#pragma once

#include "engine/ecs/system.h"
#include "engine/systems/isometric/isometricRenderItem.h"
#include "engine/systems/isometric/isometricRenderSystem.h"

namespace sfs
{

class IsometricRenderSystem;
class IsometricLightingSystem;

class IsometricShadowSystem : public System
{
public:
  void submitSpriteShadow(IsometricRenderSystem& renderSystem,
                          const IsometricRenderItem& caster,
                          const glm::vec2& spriteWorldSample,
                          int elevationLevel,
                          const IsometricLightingSystem& lightingSystem,
                          std::vector<IsometricRenderItem>& outItems);

  void submitTerrainEdgeShadows(IsometricRenderSystem& renderSystem,
                                const IsometricLightingSystem& lightingSystem,
                                std::vector<IsometricRenderItem>& outItems);

private:
  void submitTerrainShadow(std::vector<IsometricRenderItem>& outItems,
                           const glm::vec2 screenPoints[4],
                           SDL_Color tint,
                           float sortKey);

  void submitTileShadowPolygonAt(IsometricRenderSystem& renderSystem,
                                 std::vector<IsometricRenderItem>& outItems,
                                 const glm::ivec2& tile,
                                 int elevation,
                                 const glm::vec2 worldPoints[4],
                                 float alpha);

  void submitWallShadowFace(IsometricRenderSystem& renderSystem,
                            std::vector<IsometricRenderItem>& outItems,
                            const glm::ivec2& tile,
                            int elevation,
                            int side,
                            const glm::vec2 shadowWorldPoints[4],
                            float incomingElevation,
                            float alpha);

  void submitTerrainEdgeShadowProjectedClipped(
      IsometricRenderSystem& renderSystem,
      std::vector<IsometricRenderItem>& outItems,
      const TerrainShadowEdge& edge,
      const glm::vec2& shadowDir,
      float sunHeight,
      float maxShadowLength,
      float alpha);

  bool terrainShadowPathBlocked(IsometricRenderSystem& renderSystem,
                                const TerrainShadowEdge& edge,
                                const glm::ivec2& receiverTile,
                                const glm::vec2& shadowDir) const;

  static bool shouldCastTerrainShadow(const TerrainShadowEdge& edge,
                                      const glm::vec2& shadowDir);

  static void
  getWallEdge(const glm::ivec2& tile, int side, glm::vec2& a, glm::vec2& b);

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
};

} // namespace sfs
