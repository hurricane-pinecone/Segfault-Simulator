#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/geometric.hpp"

#include <cmath>

namespace sfs
{

/**
 * Where a 2D game's positions sit in the world: an entity's position on the
 * game's plane (TransformComponent::position2D()) and a height, drawn on the
 * world's XY plane (seen by a views::flat() camera), with z as depth.
 *
 * Flat: the game's plane is the world's XY plane (+Y up, unless yDown); a
 * height lifts things up the screen. Isometric: the game's plane is a grid of
 * 1 x 1 tiles drawn as diamonds (tile x down-right, tile y down-left) and
 * heights lift things up the screen. It's the picture an orthographic camera
 * looking down at the grid would see, depth and all, so the depth buffer puts
 * things in front of what they should be: a standing sprite before the
 * blocks behind it, a block's faces before those behind them.
 *
 * A 2D game's RenderSystem3D holds one (setSpace2D): its sprites, particles,
 * decals, camera and point lights are all placed through it.
 */
struct Space2D
{
  enum class Projection
  {
    Flat,
    Isometric,
  };

  Projection projection = Projection::Flat;
  // Sprite pixels per world unit (a flat game's units are world units).
  float pixelsPerUnit = 1.0f;
  // Isometric: a tile's diamond, and one unit of height, in sprite pixels.
  glm::vec2 tilePixels{32.0f, 16.0f};
  float heightPixels = 8.0f;
  // Flat: the game's y runs down the screen (as screen pixels do), not up.
  bool yDown = false;
  // Flat: things lower on the plane draw in front (a top-down game).
  bool ySort = false;

  /** Where a point of the game's plane, `height` up and `layer` forward, is. */
  glm::vec3 place(glm::vec2 position, float height = 0.0f, int layer = 0) const
  {
    if (projection == Projection::Flat)
    {
      const float up = (yDown ? -position.y : position.y) + height;
      return {position.x,
              up,
              static_cast<float>(layer) - (ySort ? up * 1.0e-4f : 0.0f)};
    }
    const Iso iso = isometric();
    const glm::vec3 point(position, height * iso.heightTiles);
    // Layers only nudge things forward here: depth already orders them.
    return glm::vec3(glm::dot(iso.right, point),
                     glm::dot(iso.up, point),
                     glm::dot(iso.toViewer, point)) *
               iso.edgePixels / pixelsPerUnit +
           glm::vec3(0.0f, 0.0f, static_cast<float>(layer) * 1.0e-3f);
  }

  /**
   * The point of the game's plane, `height` up, that's drawn at `world` (its
   * x and y): what's under the mouse, given the height it's at.
   */
  glm::vec2 unplace(glm::vec2 world, float height = 0.0f) const
  {
    if (projection == Projection::Flat)
    {
      const float up = world.y - height;
      return {world.x, yDown ? -up : up};
    }
    constexpr float kRoot2 = 1.41421356f;
    const Iso iso = isometric();
    const glm::vec2 edges = world * pixelsPerUnit / iso.edgePixels;
    const float difference = edges.x * kRoot2; // x - y
    const float sum =
        (height * iso.heightTiles * iso.cos - edges.y) * kRoot2 / iso.sin;
    return {(sum + difference) * 0.5f, (sum - difference) * 0.5f};
  }

  /** A direction on (or off) the game's plane, as the world has it. */
  glm::vec3 direction(glm::vec3 game) const
  {
    if (projection == Projection::Flat)
      return {game.x, yDown ? -game.y : game.y, game.z};
    const Iso iso = isometric();
    return {glm::dot(iso.right, game),
            glm::dot(iso.up, game),
            glm::dot(iso.toViewer, game)};
  }

  /** Whether the game's plane is the world's mirrored (meshes built through it
   * turn their triangles the other way). */
  bool mirrored() const { return projection == Projection::Isometric || yDown; }

  /**
   * How much nearer the viewer a thing standing up from the plane gets, a
   * world unit up the screen: a standing sprite leans this much, so its top
   * is in front of what's behind it, as a real one's would be.
   */
  float standingLean() const
  {
    if (projection == Projection::Flat)
      return 0.0f;
    const Iso iso = isometric();
    return iso.sin / iso.cos;
  }

  /** A length of the game's plane (a particle's size, say) in the world. */
  float scale() const
  {
    return projection == Projection::Flat
               ? 1.0f
               : isometric().edgePixels / pixelsPerUnit;
  }

  /** Which way the game's +y runs in the world (its x always runs right). */
  float ySign() const
  {
    return projection == Projection::Flat && yDown ? -1.0f : 1.0f;
  }

private:
  // The isometric camera's frame on the grid: the screen's right and up, and
  // toward the viewer (from the tiles' +x+y corner), the camera looking down
  // `asin(tile height / width)`, and a tile edge's length on screen.
  struct Iso
  {
    float sin = 0.5f;
    float cos = 0.866f;
    float edgePixels = 22.6f;
    float heightTiles = 0.4f; // a unit of height, in tile edges
    glm::vec3 right;
    glm::vec3 up;
    glm::vec3 toViewer;
  };
  Iso isometric() const
  {
    constexpr float kHalfRoot2 = 0.70710678f;
    Iso iso;
    iso.sin = tilePixels.x > 0.0f ? tilePixels.y / tilePixels.x : 0.5f;
    iso.cos = std::sqrt(std::max(1.0f - iso.sin * iso.sin, 1.0e-6f));
    iso.edgePixels = tilePixels.x * kHalfRoot2;
    iso.heightTiles = heightPixels / (iso.cos * iso.edgePixels);
    iso.right = {kHalfRoot2, -kHalfRoot2, 0.0f};
    iso.up = {-iso.sin * kHalfRoot2, -iso.sin * kHalfRoot2, iso.cos};
    iso.toViewer = {iso.cos * kHalfRoot2, iso.cos * kHalfRoot2, iso.sin};
    return iso;
  }
};

} // namespace sfs
