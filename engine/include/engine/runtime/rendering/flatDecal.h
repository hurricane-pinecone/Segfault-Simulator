#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/runtime/assetStore/sprite.h"
#include "SDL_pixels.h"
#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

/**
 * A persistent stamped quad on the flat render path -- blood splats, scorch
 * marks, grime, paint. Drawn by FlatRenderSystem each frame as a tinted,
 * rotatable textured quad, depth-sorted alongside sprites by `layer`. General
 * enough to "cover anything in blood": stamp it in world space, or attach it to
 * an entity via `follow` so it tracks a moving sprite/object.
 */
struct FlatDecal
{
  glm::vec2 worldPos{0.0f, 0.0f};
  glm::vec2 size{16.0f, 16.0f};      // world units
  glm::vec2 anchor{0.5f, 0.5f};      // 0,0 = top-left .. 1,1 = bottom-right
  float rotation = 0.0f;             // radians
  SDL_Color tint{255, 255, 255, 255};
  SpriteId sprite = 0;               // resolved via the asset store
  int layer = 0;                     // draw order (same key as RenderLayerComponent)
  float lifetime = -1.0f;            // seconds; < 0 = permanent

  // When true the decal renders at full brightness, ignoring the scene's
  // ambient/point lighting -- so vivid marks (fresh blood, paint) read clearly
  // against a dark, moodily-lit level instead of washing out to near-black.
  bool unlit = false;

  // Optional clip: when `clip` is set the decal is cropped to the world-space
  // rectangle `clipMin`..`clipMax` (e.g. the platform it stuck to), so blood
  // stamped near an edge is sliced off cleanly at the edge instead of spilling
  // past it. Applied for axis-aligned decals (rotation == 0).
  bool clip = false;
  glm::vec2 clipMin{0.0f, 0.0f};
  glm::vec2 clipMax{0.0f, 0.0f};

  // Optional: follow an entity so the decal covers a moving target. worldPos is
  // overwritten with the entity's TransformComponent position + followOffset.
  Entity follow;
  glm::vec2 followOffset{0.0f, 0.0f};

  // Optional grow: each axis whose growTo component is > 0 eases toward it at
  // growRate (world units/sec). Drives e.g. a blood streak running down a face.
  glm::vec2 growTo{0.0f, 0.0f};
  float growRate = 0.0f;
};

} // namespace sfs
