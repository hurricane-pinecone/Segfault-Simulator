#include "engine/runtime/rendering/modules/blockGeometry.h"

#include "SDL2/SDL_surface.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/assetStore/sprite.h"
#include "engine/core/components/elevationComponent.h"
#include "engine/core/components/spriteComponent.h"
#include "engine/core/components/surfaceEffect.h"
#include "engine/core/components/tags/isometricTile.h"
#include "engine/core/components/terrainBoundaryComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/registry.h" // IWYU pragma: keep -- Entity::getComponent<T> defs
#include "engine/runtime/rendering/renderPass.h"
#include "engine/core/util/profiling.h"
#include "glm/glm/ext/vector_float3.hpp"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sfs
{

namespace
{

// Elevation's weight in the painter sort-key, matching the billboard tile sort
// in IsometricRenderSystem so geometry and sprites (actors) interleave the same.
constexpr float kElevationSortWeight = 0.5f;

// A cube sprite (e.g. block.png) packs a 2:1 top diamond over two side faces.
// These are fractional coordinates within the sprite's srcRect: the top diamond
// fills the upper half, the left/right faces the lower-left / lower-right.
struct UvF
{
  float x;
  float y;
};

// Top diamond corners (fraction of the sprite).
constexpr UvF kDiamondTop{0.5f, 0.0f};
constexpr UvF kDiamondRight{1.0f, 0.25f};
constexpr UvF kDiamondBottom{0.5f, 0.5f};
constexpr UvF kDiamondLeft{0.0f, 0.25f};

// Left face (the screen-left wall = +y / south edge), one elevation level tall.
constexpr UvF kLeftTopOuter{0.0f, 0.25f};  // along the diamond's left edge
constexpr UvF kLeftTopInner{0.5f, 0.5f};   // the diamond's bottom vertex
constexpr UvF kLeftBotInner{0.5f, 1.0f};
constexpr UvF kLeftBotOuter{0.0f, 0.75f};

// Right face (the screen-right wall = +x / east edge), one elevation level tall.
constexpr UvF kRightTopInner{0.5f, 0.5f};  // the diamond's bottom vertex
constexpr UvF kRightTopOuter{1.0f, 0.25f}; // the diamond's right vertex
constexpr UvF kRightBotOuter{1.0f, 0.75f};
constexpr UvF kRightBotInner{0.5f, 1.0f};

} // namespace

glm::vec2 BlockGeometry::textureSize(const std::string& textureId)
{
  if (const auto it = m_textureSizeCache.find(textureId);
      it != m_textureSizeCache.end())
    return it->second;

  glm::vec2 size{1.0f, 1.0f};

  if (SDL_Surface* surface = m_assetStore->getSurface(textureId))
    size = {static_cast<float>(surface->w), static_cast<float>(surface->h)};

  m_textureSizeCache.emplace(textureId, size);
  return size;
}

void BlockGeometry::computeCommands(const IsometricRenderContext& context)
{
  ZoneScopedN("BlockGeometry::computeCommands");

  flush();
  m_textureSizeCache.clear();

  if (!context.projection)
    return;

  // One accumulating triangle list per material (texture + surface effect), so a
  // whole material draws in a single GeometryCommand.
  struct Bucket
  {
    const std::string* textureId = nullptr;
    SurfaceEffect::Type effect = SurfaceEffect::Type::None;
    std::vector<GeometryVertex> vertices;
  };
  std::unordered_map<std::string, Bucket> buckets;

  // Project a grid corner (with elevation in levels) to screen pixels.
  const auto ws = [&](float gx, float gy, float elev)
  { return context.worldToScreen({gx, gy}, elev); };

  // Push a quad (4 screen corners + their world XY, elevation, uv) as two
  // triangles into `out`, stamping each vertex's painter sort-key into z.
  const auto pushQuad = [](std::vector<GeometryVertex>& out,
                           const glm::vec2 pos[4],
                           const glm::vec2 world[4],
                           const float ground[4],
                           const glm::vec2 uv[4],
                           const glm::vec3& normal)
  {
    GeometryVertex v[4];
    for (int i = 0; i < 4; i++)
    {
      v[i].position = pos[i];
      v[i].worldPos = world[i];
      v[i].ground = ground[i];
      v[i].uv = uv[i];
      v[i].normal = normal;
      v[i].z = world[i].x + world[i].y + ground[i] * kElevationSortWeight;
    }

    // 0-1-2, 0-2-3 (winding is irrelevant; faces are not back-culled).
    out.push_back(v[0]);
    out.push_back(v[1]);
    out.push_back(v[2]);
    out.push_back(v[0]);
    out.push_back(v[2]);
    out.push_back(v[3]);
  };

  for (const auto& entity :
       registry->view<TransformComponent,
                      ElevationComponent,
                      SpriteComponent,
                      IsometricTile>())
  {
    const auto& transform = entity.getComponent<TransformComponent>();
    const int elevation = entity.getComponent<ElevationComponent>().level;
    const auto& spriteComponent = entity.getComponent<SpriteComponent>();

    const Sprite* sprite = m_assetStore->getSprite(spriteComponent.spriteId);
    if (!sprite)
      continue;

    const SDL_Rect& src = sprite->srcRect;
    const glm::vec2 texSize = textureSize(sprite->textureId);

    // srcRect fraction -> normalised texture UV.
    const auto uvOf = [&](const UvF& f)
    {
      return glm::vec2{
          (static_cast<float>(src.x) + f.x * static_cast<float>(src.w)) /
              texSize.x,
          (static_cast<float>(src.y) + f.y * static_cast<float>(src.h)) /
              texSize.y,
      };
    };

    const SurfaceEffect::Type effect =
        entity.hasComponent<SurfaceEffect>()
            ? entity.getComponent<SurfaceEffect>().type
            : SurfaceEffect::Type::None;

    const std::string key =
        sprite->textureId + '|' + std::to_string(static_cast<int>(effect));

    Bucket& bucket = buckets[key];
    bucket.textureId = &sprite->textureId;
    bucket.effect = effect;

    const float tx = transform.position.x;
    const float ty = transform.position.y;
    const float top = static_cast<float>(elevation);

    // --- Top face: the diamond at the tile's elevation. ---
    {
      const glm::vec2 pos[4] = {
          ws(tx, ty, top),               // diamond top
          ws(tx + 1.0f, ty, top),        // diamond right
          ws(tx + 1.0f, ty + 1.0f, top), // diamond bottom
          ws(tx, ty + 1.0f, top),        // diamond left
      };
      const glm::vec2 world[4] = {
          {tx, ty},
          {tx + 1.0f, ty},
          {tx + 1.0f, ty + 1.0f},
          {tx, ty + 1.0f},
      };
      const float ground[4] = {top, top, top, top};
      const glm::vec2 uv[4] = {
          uvOf(kDiamondTop),
          uvOf(kDiamondRight),
          uvOf(kDiamondBottom),
          uvOf(kDiamondLeft),
      };
      pushQuad(bucket.vertices, pos, world, ground, uv, {0.0f, 0.0f, 1.0f});
    }

    // Only tiles on a height boundary carry exposed-side data. The camera sees
    // just the +y (south) and +x (east) faces; the far faces are always behind
    // the block's own silhouette, so they're skipped (as the billboard did).
    if (!entity.hasComponent<TerrainBoundaryComponent>())
      continue;

    const auto& boundary = entity.getComponent<TerrainBoundaryComponent>();

    // --- South face (+y edge, screen-left = sprite's left face). ---
    // One quad per elevation level so the side art tiles down the drop.
    for (int level = elevation; boundary.southExposed &&
                                level > boundary.southBottomElevation;
         level--)
    {
      const float t = static_cast<float>(level);
      const float b = static_cast<float>(level - 1);

      const glm::vec2 pos[4] = {
          ws(tx, ty + 1.0f, t),        // top, screen-left
          ws(tx + 1.0f, ty + 1.0f, t), // top, screen-bottom
          ws(tx + 1.0f, ty + 1.0f, b), // bottom, screen-bottom
          ws(tx, ty + 1.0f, b),        // bottom, screen-left
      };
      const glm::vec2 world[4] = {
          {tx, ty + 1.0f},
          {tx + 1.0f, ty + 1.0f},
          {tx + 1.0f, ty + 1.0f},
          {tx, ty + 1.0f},
      };
      const float ground[4] = {t, t, b, b};
      const glm::vec2 uv[4] = {
          uvOf(kLeftTopOuter),
          uvOf(kLeftTopInner),
          uvOf(kLeftBotInner),
          uvOf(kLeftBotOuter),
      };
      pushQuad(bucket.vertices, pos, world, ground, uv, {0.0f, 1.0f, 0.0f});
    }

    // --- East face (+x edge, screen-right = sprite's right face). ---
    for (int level = elevation;
         boundary.eastExposed && level > boundary.eastBottomElevation; level--)
    {
      const float t = static_cast<float>(level);
      const float b = static_cast<float>(level - 1);

      const glm::vec2 pos[4] = {
          ws(tx + 1.0f, ty + 1.0f, t), // top, screen-bottom
          ws(tx + 1.0f, ty, t),        // top, screen-right
          ws(tx + 1.0f, ty, b),        // bottom, screen-right
          ws(tx + 1.0f, ty + 1.0f, b), // bottom, screen-bottom
      };
      const glm::vec2 world[4] = {
          {tx + 1.0f, ty + 1.0f},
          {tx + 1.0f, ty},
          {tx + 1.0f, ty},
          {tx + 1.0f, ty + 1.0f},
      };
      const float ground[4] = {t, t, b, b};
      const glm::vec2 uv[4] = {
          uvOf(kRightTopInner),
          uvOf(kRightTopOuter),
          uvOf(kRightBotOuter),
          uvOf(kRightBotInner),
      };
      pushQuad(bucket.vertices, pos, world, ground, uv, {1.0f, 0.0f, 0.0f});
    }
  }

  m_commands.reserve(buckets.size());
  for (auto& [key, bucket] : buckets)
  {
    if (bucket.vertices.empty())
      continue;

    GeometryCommand command;
    command.order = RenderOrder{RenderPass::Terrain, 0, 0};
    command.textureId = bucket.textureId;
    command.type = bucket.effect;
    command.vertices = std::move(bucket.vertices);

    m_commands.push_back(std::move(command));
  }
}

} // namespace sfs
