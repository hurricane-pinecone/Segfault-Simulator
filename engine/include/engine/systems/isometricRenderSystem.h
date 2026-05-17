#pragma once

#include "engine/assetStore/assetStore.h"
#include "engine/components/cameraComponent.h"
#include "engine/components/elevationComponent.h"
#include "engine/components/transformComponent.h"
#include "engine/ecs/system.h"

#include "engine/renderers/renderContext.h"
#include "engine/systems/isometricLightingSystem.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_int2.hpp"
#include "glm/glm/geometric.hpp"

#include <SDL_pixels.h>
#include <SDL_rect.h>

#include <algorithm>
#include <string>
#include <vector>

namespace sfs
{

constexpr int WallSideRightVisible = 2;
constexpr int WallSideFrontVisible = 3;

constexpr float ShadowLength = 3.0f;
constexpr float ShadowStepSize = 0.35f;
constexpr int MaxShadowSteps = 12;
constexpr float ShadowAlpha = 0.45f;
constexpr float WallShadowAlpha = 90.0f / 255.0f;
constexpr float CasterFootWidth = 0.35f;
constexpr float CasterFootDepth = 0.22f;

struct ShadowWallFace
{
  enum class Type
  {
    West,
    North,
  };

  glm::ivec2 tile;
  int elevation = 0;
  Type type;
};

struct ActiveCamera
{
  const CameraComponent* camera = nullptr;
  const TransformComponent* transform = nullptr;
};

class IsometricRenderSystem : public System
{
public:
  IsometricRenderSystem(AssetStore& assetStore,
                        int windowWidth,
                        int windowHeight,
                        int tileWidth,
                        int tileHeight);

  ~IsometricRenderSystem();

  void render();

  void setWaveTime(float time);
  void setWaveEnabled(bool enabled);

  void drawDebugTile(const glm::vec2& gridPosition,
                     SDL_Color color = SDL_Color{255, 255, 0, 255});

  void drawDebugTile(const glm::vec2& gridPosition,
                     int elevation,
                     SDL_Color color = SDL_Color{255, 255, 0, 255});

  glm::vec2 gridToIsometric(const glm::vec2& gridPosition) const;
  glm::vec2 isometricToGrid(const glm::vec2& iso) const;

  void setWorldScale(float scale);
  float getWorldScale() const;

  IsometricRenderSystem(const IsometricRenderSystem&) = delete;
  IsometricRenderSystem& operator=(const IsometricRenderSystem&) = delete;

private:
  struct RenderItem
  {
    const std::string* textureId = nullptr;

    SDL_Rect srcRect{0, 0, 0, 0};
    SDL_Rect dest{0, 0, 0, 0};

    int textureWidth = 0;
    int textureHeight = 0;

    float sortKey = 0.0f;
    int renderLayer = 1;
    float screenSortY = 0.0f;

    bool hasNormalMap = false;
    const std::string* normalTextureId = nullptr;
    SDL_Rect normalSrcRect{0, 0, 0, 0};
    int normalTextureWidth = 0;
    int normalTextureHeight = 0;

    IsometricComputedLighting lighting{};

    bool isShadow = false;
    glm::vec2 shadowOffset{0.0f, 0.0f};
    bool isTerrainShadow = false;

    glm::vec2 shadowScreenPoints[4] = {};

    SDL_Color tint{255, 255, 255, 255};
    glm::vec2 worldPoints[4] = {
        glm::vec2{0.0f, 0.0f},
        glm::vec2{0.0f, 0.0f},
        glm::vec2{0.0f, 0.0f},
        glm::vec2{0.0f, 0.0f},
    };
  };

  void beginBatches();
  void submitSprite(const RenderItem& item);
  void submitShadow(const RenderItem& caster,
                    const glm::vec2& shadowOffset,
                    float alpha,
                    float sortKeyBias = 0.005f);
  void submitTerrainShadow(const glm::vec2 screenPoints[4],
                           SDL_Color tint,
                           float sortKey,
                           const std::string* textureId,
                           SDL_Rect srcRect,
                           int textureWidth,
                           int textureHeight);
  void flushBatches();

  void submitTileShadowAt(const glm::ivec2& tile,
                          int elevation,
                          float alpha,
                          const RenderItem& caster)
  {
    const auto& activeCamera = getCamera();
    const float zoom = activeCamera.camera ? activeCamera.camera->zoom : 1.0f;

    const glm::vec2 screenCenter{
        static_cast<float>(windowWidth) / 2.0f,
        static_cast<float>(windowHeight) / 2.0f,
    };

    const glm::vec2 isoCameraPosition = gridToIsometric(getCameraPosition());

    auto tilePointToScreen = [&](glm::vec2 p)
    {
      glm::vec2 screen =
          (gridToIsometric(p) - isoCameraPosition) * zoom + screenCenter;

      screen.y -= elevation * elevationStep * worldScale * zoom;
      screen.y -= getWaveOffset(p) * zoom;

      return screen;
    };

    glm::vec2 points[4] = {
        tilePointToScreen(glm::vec2{tile.x + 0.0f, tile.y + 0.0f}),
        tilePointToScreen(glm::vec2{tile.x + 1.0f, tile.y + 0.0f}),
        tilePointToScreen(glm::vec2{tile.x + 1.0f, tile.y + 1.0f}),
        tilePointToScreen(glm::vec2{tile.x + 0.0f, tile.y + 1.0f}),
    };

    constexpr float ElevationSortWeight = 0.5f;

    const float sortKey =
        static_cast<float>(tile.x) + static_cast<float>(tile.y) +
        static_cast<float>(elevation) * ElevationSortWeight + 0.0005f;
    submitTerrainShadow(points,
                        SDL_Color{255, 0, 0, 220},
                        sortKey,
                        caster.textureId,
                        caster.srcRect,
                        caster.textureWidth,
                        caster.textureHeight);
  }

  void submitTileShadowPolygonAt(const glm::ivec2& tile,
                                 int elevation,
                                 const glm::vec2 worldPoints[4],
                                 float alpha,
                                 const RenderItem& caster)
  {
    const std::vector<glm::vec2> clipped = clipPolygonToTile(worldPoints, tile);

    if (clipped.size() < 3)
      return;

    const auto& activeCamera = getCamera();
    const float zoom = activeCamera.camera ? activeCamera.camera->zoom : 1.0f;

    const glm::vec2 screenCenter{
        static_cast<float>(windowWidth) / 2.0f,
        static_cast<float>(windowHeight) / 2.0f,
    };

    const glm::vec2 isoCameraPosition = gridToIsometric(getCameraPosition());

    auto worldPointToScreen = [&](const glm::vec2& p)
    {
      glm::vec2 screen =
          (gridToIsometric(p) - isoCameraPosition) * zoom + screenCenter;

      screen.y -= elevation * elevationStep * worldScale * zoom;
      screen.y -= getWaveOffset(p) * zoom;

      return screen;
    };

    constexpr float ElevationSortWeight = 0.5f;

    const float sortKey =
        static_cast<float>(tile.x) + static_cast<float>(tile.y) +
        static_cast<float>(elevation) * ElevationSortWeight + 0.0005f;

    for (size_t i = 1; i + 1 < clipped.size(); i++)
    {
      glm::vec2 screenPoints[4] = {
          worldPointToScreen(clipped[0]),
          worldPointToScreen(clipped[i]),
          worldPointToScreen(clipped[i + 1]),
          worldPointToScreen(clipped[i + 1]),
      };

      submitTerrainShadow(
          screenPoints,
          SDL_Color{0, 0, 0, static_cast<Uint8>(alpha * 255.0f)},
          sortKey,
          caster.textureId,
          caster.srcRect,
          caster.textureWidth,
          caster.textureHeight);
    }
  }
  static std::vector<glm::vec2>
  clipPolygonAgainstEdge(const std::vector<glm::vec2>& input,
                         float edge,
                         int axis,
                         bool keepGreater)
  {
    std::vector<glm::vec2> output;

    if (input.empty())
      return output;

    auto inside = [&](const glm::vec2& p)
    {
      const float value = axis == 0 ? p.x : p.y;
      return keepGreater ? value >= edge : value <= edge;
    };

    auto intersect = [&](const glm::vec2& a, const glm::vec2& b)
    {
      const float av = axis == 0 ? a.x : a.y;
      const float bv = axis == 0 ? b.x : b.y;

      const float t = (edge - av) / (bv - av);

      return a + (b - a) * t;
    };

    glm::vec2 previous = input.back();
    bool previousInside = inside(previous);

    for (const glm::vec2& current : input)
    {
      const bool currentInside = inside(current);

      if (currentInside)
      {
        if (!previousInside)
          output.push_back(intersect(previous, current));

        output.push_back(current);
      }
      else if (previousInside)
      {
        output.push_back(intersect(previous, current));
      }

      previous = current;
      previousInside = currentInside;
    }

    return output;
  }

  static std::vector<glm::vec2>
  clipPolygonToTile(const glm::vec2 worldPoints[4], const glm::ivec2& tile)
  {
    std::vector<glm::vec2> polygon = {
        worldPoints[0],
        worldPoints[1],
        worldPoints[2],
        worldPoints[3],
    };

    const float minX = static_cast<float>(tile.x);
    const float maxX = static_cast<float>(tile.x + 1);
    const float minY = static_cast<float>(tile.y);
    const float maxY = static_cast<float>(tile.y + 1);

    polygon = clipPolygonAgainstEdge(polygon, minX, 0, true);
    polygon = clipPolygonAgainstEdge(polygon, maxX, 0, false);
    polygon = clipPolygonAgainstEdge(polygon, minY, 1, true);
    polygon = clipPolygonAgainstEdge(polygon, maxY, 1, false);

    return polygon;
  }

  std::vector<ShadowWallFace> getShadowWallFacesAround(const glm::ivec2& center,
                                                       int radius)
  {
    std::vector<ShadowWallFace> faces;

    for (int y = center.y - radius; y <= center.y + radius; y++)
    {
      for (int x = center.x - radius; x <= center.x + radius; x++)
      {
        const int elevation = getTileElevationAt(glm::vec2{x, y});

        if (elevation <= 0)
          continue;

        const int westElevation = getTileElevationAt(glm::vec2{x - 1, y});

        const int northElevation = getTileElevationAt(glm::vec2{x, y - 1});

        if (westElevation < elevation)
        {
          faces.push_back({
              glm::ivec2{x, y},
              elevation,
              ShadowWallFace::Type::West,
          });
        }

        if (northElevation < elevation)
        {
          faces.push_back({
              glm::ivec2{x, y},
              elevation,
              ShadowWallFace::Type::North,
          });
        }
      }
    }

    return faces;
  }

  void drawDebugWallFace(const glm::ivec2& tile,
                         int elevation,
                         int side,
                         SDL_Color color)
  {
    const auto& activeCamera = getCamera();
    const float zoom = activeCamera.camera ? activeCamera.camera->zoom : 1.0f;

    const glm::vec2 screenCenter{
        static_cast<float>(windowWidth) / 2.0f,
        static_cast<float>(windowHeight) / 2.0f,
    };

    const glm::vec2 isoCameraPosition = gridToIsometric(getCameraPosition());

    auto worldToScreen = [&](glm::vec2 p, float z)
    {
      glm::vec2 screen =
          (gridToIsometric(p) - isoCameraPosition) * zoom + screenCenter;

      screen.y -= z * elevationStep * worldScale * zoom;
      return screen;
    };

    glm::vec2 a;
    glm::vec2 b;

    if (side == 0)
    {
      a = glm::vec2{tile.x, tile.y};
      b = glm::vec2{tile.x, tile.y + 1};
    }
    else if (side == 1)
    {
      a = glm::vec2{tile.x, tile.y};
      b = glm::vec2{tile.x + 1, tile.y};
    }
    else if (side == 2)
    {
      a = glm::vec2{tile.x + 1, tile.y};
      b = glm::vec2{tile.x + 1, tile.y + 1};
    }
    else
    {
      a = glm::vec2{tile.x, tile.y + 1};
      b = glm::vec2{tile.x + 1, tile.y + 1};
    }

    glm::vec2 points[5] = {
        worldToScreen(a, elevation),
        worldToScreen(b, elevation),
        worldToScreen(b, elevation - 1),
        worldToScreen(a, elevation - 1),
        worldToScreen(a, elevation),
    };

    RenderContext::quadRenderer().drawLineLoop(points, 5, color);
  }

  void submitWallShadowFace(const glm::ivec2& tile,
                            int elevation,
                            int side,
                            const glm::vec2 shadowWorldPoints[4],
                            float incomingElevation,
                            float normalizedDistance,
                            float alpha,
                            const RenderItem& textureSource)
  {
    const auto& activeCamera = getCamera();
    const float zoom = activeCamera.camera ? activeCamera.camera->zoom : 1.0f;

    const glm::vec2 screenCenter{
        static_cast<float>(windowWidth) / 2.0f,
        static_cast<float>(windowHeight) / 2.0f,
    };

    const glm::vec2 isoCameraPosition = gridToIsometric(getCameraPosition());

    auto worldToScreen = [&](glm::vec2 p, float z)
    {
      glm::vec2 screen =
          (gridToIsometric(p) - isoCameraPosition) * zoom + screenCenter;

      screen.y -= z * elevationStep * worldScale * zoom;
      return screen;
    };

    glm::vec2 a;
    glm::vec2 b;

    if (side == 0)
    {
      a = glm::vec2{tile.x, tile.y};
      b = glm::vec2{tile.x, tile.y + 1};
    }
    else if (side == 1)
    {
      a = glm::vec2{tile.x, tile.y};
      b = glm::vec2{tile.x + 1, tile.y};
    }
    else if (side == 2)
    {
      a = glm::vec2{tile.x + 1, tile.y};
      b = glm::vec2{tile.x + 1, tile.y + 1};
    }
    else
    {
      a = glm::vec2{tile.x, tile.y + 1};
      b = glm::vec2{tile.x + 1, tile.y + 1};
    }

    float minT = 0.0f;
    float maxT = 1.0f;

    if (!projectShadowOntoWallEdge(shadowWorldPoints, a, b, minT, maxT))
      return;

    constexpr float WallPad = 0.05f;

    minT = std::clamp(minT - WallPad, 0.0f, 1.0f);
    maxT = std::clamp(maxT + WallPad, 0.0f, 1.0f);

    const glm::vec2 shadowA = a + (b - a) * minT;
    const glm::vec2 shadowB = a + (b - a) * maxT;

    const float topZ = static_cast<float>(elevation);

    const float incomingZ = std::clamp(incomingElevation, 0.0f, topZ);

    const float elevationDelta = std::max(0.0f, topZ - incomingZ);

    const float shadowTopZ = topZ;

    // For upward hits, cover the whole transition immediately.
    // The ray already paid the climbCost, so the wall should represent that
    // climb.
    const float shadowBottomZ = incomingZ;

    glm::vec2 screenPoints[4] = {
        worldToScreen(shadowA, shadowTopZ),
        worldToScreen(shadowB, shadowTopZ),
        worldToScreen(shadowB, shadowBottomZ),
        worldToScreen(shadowA, shadowBottomZ),
    };

    const float sortKey = static_cast<float>(tile.x) +
                          static_cast<float>(tile.y) +
                          static_cast<float>(elevation) * 0.5f + 0.0006f;

    submitTerrainShadow(
        screenPoints,
        SDL_Color{255, 0, 255, 220}, // keep magenta until both faces show
        sortKey,
        textureSource.textureId,
        textureSource.srcRect,
        textureSource.textureWidth,
        textureSource.textureHeight);
  }

  static void
  getWallEdge(const glm::ivec2& tile, int side, glm::vec2& a, glm::vec2& b)
  {
    if (side == 0)
    {
      a = glm::vec2{tile.x, tile.y};
      b = glm::vec2{tile.x, tile.y + 1};
    }
    else if (side == 1)
    {
      a = glm::vec2{tile.x, tile.y};
      b = glm::vec2{tile.x + 1, tile.y};
    }
    else if (side == 2)
    {
      a = glm::vec2{tile.x + 1, tile.y};
      b = glm::vec2{tile.x + 1, tile.y + 1};
    }
    else
    {
      a = glm::vec2{tile.x, tile.y + 1};
      b = glm::vec2{tile.x + 1, tile.y + 1};
    }
  }

  static bool segmentIntersectsSegment(glm::vec2 p1,
                                       glm::vec2 p2,
                                       glm::vec2 q1,
                                       glm::vec2 q2)
  {
    auto cross = [](glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; };

    const glm::vec2 r = p2 - p1;
    const glm::vec2 s = q2 - q1;

    const float denom = cross(r, s);

    if (std::abs(denom) < 0.0001f)
      return false;

    const float t = cross(q1 - p1, s) / denom;
    const float u = cross(q1 - p1, r) / denom;

    return t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f;
  }

  bool shadowTouchesWallFace(const glm::vec2 shadowPoly[4],
                             const glm::ivec2& tile,
                             int side)
  {
    glm::vec2 wallA;
    glm::vec2 wallB;

    if (side == 0)
    {
      wallA = glm::vec2{tile.x, tile.y};
      wallB = glm::vec2{tile.x, tile.y + 1};
    }
    else if (side == 1)
    {
      wallA = glm::vec2{tile.x, tile.y};
      wallB = glm::vec2{tile.x + 1, tile.y};
    }
    else if (side == 2)
    {
      wallA = glm::vec2{tile.x + 1, tile.y};
      wallB = glm::vec2{tile.x + 1, tile.y + 1};
    }
    else // side == 3
    {
      wallA = glm::vec2{tile.x, tile.y + 1};
      wallB = glm::vec2{tile.x + 1, tile.y + 1};
    }

    constexpr float TouchEpsilon = 0.08f;

    auto pointNearSegment = [&](glm::vec2 p, glm::vec2 a, glm::vec2 b)
    {
      const glm::vec2 ab = b - a;
      const float abLen2 = glm::dot(ab, ab);

      if (abLen2 < 0.0001f)
        return false;

      const float t = std::clamp(glm::dot(p - a, ab) / abLen2, 0.0f, 1.0f);

      const glm::vec2 closest = a + ab * t;

      return glm::length(p - closest) <= TouchEpsilon;
    };

    for (int i = 0; i < 4; i++)
    {
      const glm::vec2 a = shadowPoly[i];
      const glm::vec2 b = shadowPoly[(i + 1) % 4];

      if (segmentIntersectsSegment(a, b, wallA, wallB))
        return true;

      if (pointNearSegment(a, wallA, wallB))
        return true;
    }

    return false;
  }

  static bool pointInConvexQuad(glm::vec2 p, const glm::vec2 quad[4])
  {
    auto cross = [](glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; };

    bool hasPositive = false;
    bool hasNegative = false;

    for (int i = 0; i < 4; i++)
    {
      const glm::vec2 a = quad[i];
      const glm::vec2 b = quad[(i + 1) % 4];

      const float c = cross(b - a, p - a);

      if (c > 0.0001f)
        hasPositive = true;

      if (c < -0.0001f)
        hasNegative = true;
    }

    return !(hasPositive && hasNegative);
  }

  static bool projectShadowOntoWallEdge(const glm::vec2 shadowPoly[4],
                                        glm::vec2 wallA,
                                        glm::vec2 wallB,
                                        float& outMinT,
                                        float& outMaxT)
  {
    outMinT = 1.0f;
    outMaxT = 0.0f;

    const glm::vec2 wall = wallB - wallA;
    const float wallLen2 = glm::dot(wall, wall);

    if (wallLen2 < 0.0001f)
      return false;

    auto wallT = [&](glm::vec2 p)
    { return std::clamp(glm::dot(p - wallA, wall) / wallLen2, 0.0f, 1.0f); };

    auto addT = [&](float t)
    {
      outMinT = std::min(outMinT, t);
      outMaxT = std::max(outMaxT, t);
    };

    // Sample along the wall edge. This catches angled cases where the wall edge
    // lies inside the shadow polygon instead of crossing its boundary.
    constexpr int Samples = 12;

    for (int i = 0; i <= Samples; i++)
    {
      const float t = static_cast<float>(i) / static_cast<float>(Samples);
      const glm::vec2 p = wallA + wall * t;

      if (pointInConvexQuad(p, shadowPoly))
        addT(t);
    }

    return outMaxT > outMinT;
  }

  glm::vec2 getCameraPosition() const;
  glm::ivec2 gridCellOf(const glm::vec2& position) const;

  bool isTileEntity(const Entity& entity) const;

  int getRenderElevationLevel(const Entity& entity,
                              const glm::vec2& samplePosition) const;

  glm::vec2 getGroundSamplePosition(const Entity& entity,
                                    const TransformComponent& transform) const;

  bool tryGetTileElevationAt(const glm::vec2& position,
                             int& outElevation) const;

  int getTileElevationAt(const glm::vec2& position) const;

  float getWaveOffset(const glm::vec2& gridPosition) const;

  ActiveCamera getCamera() const;

  static std::string tileKey(const glm::ivec2& tile)
  {
    return std::to_string(tile.x) + "," + std::to_string(tile.y);
  }

  void rebuildTileElevationCache()
  {
    tileElevationCache.clear();

    for (const auto& entity : getEntities())
    {
      if (!isTileEntity(entity))
        continue;

      const auto& transform = entity.getComponent<TransformComponent>();

      int elevation = 0;

      if (entity.hasComponent<ElevationComponent>())
        elevation = entity.getComponent<ElevationComponent>().level;

      const glm::ivec2 tile = gridCellOf(transform.position);
      const std::string key = tileKey(tile);

      auto it = tileElevationCache.find(key);

      if (it == tileElevationCache.end())
        tileElevationCache.emplace(key, elevation);
      else
        it->second = std::max(it->second, elevation);
    }
  }

private:
  AssetStore& assetStore;

  int windowWidth = 0;
  int windowHeight = 0;

  float worldScale = 1.0f;

  int tileWidth = 0;
  int tileHeight = 0;

  int elevationStep = 8;
  std::unordered_map<std::string, int> tileElevationCache;

  bool waveEnabled = false;
  float waveTime = 0.0f;
  float waveAmplitude = 6.0f;
  float waveFrequency = 0.45f;
  float waveSpeed = 3.0f;

  std::vector<RenderItem> renderItems;
};

} // namespace sfs
