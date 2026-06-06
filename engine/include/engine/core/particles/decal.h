#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include <cstdint>
#include <string>

namespace sfs
{

// Which terrain surface a decal sits on (set from particle-vs-terrain
// collision).
enum class DecalSurface : uint8_t
{
  Ground, // flat on a tile top
  Wall,   // on a vertical block face (see wallSide)
  Water,  // floating on a water surface (fades; see fadeRate)
};

// How a decal is bounded to the surface it sits on. Default keeps it on that
// surface (a mark never spills past a platform edge); None lets it extend freely
// (e.g. a streak meant to trail off an edge).
enum class Clipping : uint8_t
{
  Surface,
  None,
};

// A request to lay down one persistent mark on the terrain, produced when a
// particle collides with the world and consumed by an IDecalSink.
struct DecalSpawn
{
  glm::vec2 worldPos{0.0f, 0.0f};
  float elevation = 0.0f; // surface elevation level it landed on

  DecalSurface surface = DecalSurface::Ground;
  uint8_t wallSide = 0;    // 0=W 1=N 2=E 3=S, only when surface == Wall
  float wallBottom = 0.0f; // elevation of the wall face's base (Wall only)
  float wallTop = 0.0f;    // elevation of the wall face's top (Wall only)

  // Footprint in tiles. size.x = length (along `rotation`'s local +X axis),
  // size.y = width (across). Equal = a round splat; x > y = a directional streak.
  // Wall drips use size.y as the streak width (length comes from the elevation
  // run, not size).
  glm::vec2 size{0.15f, 0.15f};
  float rotation = 0.0f;
  glm::vec4 color{0.35f, 0.0f, 0.0f, 0.85f};

  // Points at the effect's decal texture id; valid only for the addDecal() call
  // (the sink interns it). null falls back to the sink's default.
  const std::string* textureId = nullptr;

  // Per-second alpha decay. 0 = permanent (ground); > 0 = fades (water, wall
  // drips).
  float fadeRate = 0.0f;

  // Wall drips only: how fast (elevation levels/sec) the streak runs down the
  // face. `elevation` is the streak's top; the head descends toward wallBottom.
  float dripSpeed = 0.0f;

  // Keep the mark on the surface it hit (clip to clipMin..clipMax, world-space).
  // Default clips; set None to let it extend past the rect. The flat path fills
  // the rect from the collider it stuck to; the isometric sink ignores this and
  // clips per tile/face.
  Clipping clipping = Clipping::Surface;
  glm::vec2 clipMin{0.0f, 0.0f};
  glm::vec2 clipMax{0.0f, 0.0f};

  // Hint for sinks that keep a hard + soft sprite: true = a crisp streak (hard
  // edge), false = a soft area blob. The isometric sink ignores it.
  bool crisp = false;
};

// Receives decals from collisions. A render path's decal sink implements this;
// the Particles module holds a pointer to it (bridged by the scene, so neither
// depends on the other).
class IDecalSink
{
public:
  virtual ~IDecalSink() = default;
  virtual void addDecal(const DecalSpawn& spawn) = 0;
};

} // namespace sfs
