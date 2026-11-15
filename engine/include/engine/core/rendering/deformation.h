#pragma once

#include "engine/core/rendering/mesh.h"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include "glm/glm/ext/vector_int2.hpp"

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sfs
{

struct TextureData;

/**
 * Ground pressed in and pushed up: tyre tracks, ruts, furrows, footprints on
 * a deformable terrain (TerrainMaterial::deformable; see DeformationModule,
 * which draws it and feeds it from DeformerComponent entities).
 *
 * A DeformationMap is how far the ground at each texel of the world's XY is
 * off the terrain's own surface (metres: negative pressed down, positive
 * pushed up), kDeformationTexel across, kept in tiles of kTile x kTile made
 * only where something has touched the ground: a big field costs nothing
 * until it's driven over, and memory grows with the tracks, not the land
 * (about 16 KB a tile, 1.9 m square).
 *
 * Everything that changes it goes through press(): for each texel touched,
 * the offset it's pressed down to. Soil pressed out of the way is pushed up
 * alongside as a ridge (a share of it, the rest compacted), to the sides of
 * the way it's going, and walls steeper than soil stands slump. Nothing
 * presses deeper than kDeepest.
 *
 * It's plain data on the CPU: games read it (vehicles settling into ruts),
 * the puddles are found with it, and save() / load() keep it with a saved
 * game.
 */

/** World units across a deformation texel (about a tyre lug). */
inline constexpr float kDeformationTexel = 0.03f;
/** The deepest the ground is pressed (metres below its own surface). */
inline constexpr float kDeepest = 0.5f;

/**
 * Texels a deformer (or a stamp) presses, over a rectangle of the map's
 * texels: for each, the offset it's pressed down to (NaN: not under it), and
 * how much of the texel it covers (its edges' texels pressed part way, so
 * they slope rather than step; empty: all of each).
 */
struct Footprint
{
  glm::ivec2 from{0}; // the rectangle's first texel (world texel index)
  glm::ivec2 size{0};
  std::vector<float> target;
  std::vector<float> coverage;

  bool empty() const { return size.x <= 0 || size.y <= 0; }
};

/** How press() pushes displaced soil up. */
struct PressSettings
{
  float ridge = 0.5f;       // the share of what's pressed out pushed up, 0..1
  float ridgeWidth = 0.12f; // how far out from the pressed texels (m)
  float ridgeMost = 0.05f;  // the highest a ridge gets (m above the surface)
  // The steepest the soil around stands (rise over run): steeper, it slumps.
  float steepest = 1.2f;
  // Which way the deformer moves (world XY, any length; zero: still): its
  // ridge goes mostly to the sides.
  glm::vec2 motion{0.0f};
  // A tyre's tread (how far its lugs stand proud, metres; 0: none):
  // slumping leaves the bottom this deep of the ground it's on alone (a
  // rut's floor, its lug marks), so walls slump, not its tread.
  float tread = 0.0f;
};

class DeformationMap
{
public:
  static constexpr int kTile = 64;

  explicit DeformationMap(float texel = kDeformationTexel);

  float texel() const { return m_texel; }
  /** The texel a world XY point is in. */
  glm::ivec2 texelOf(glm::vec2 point) const;
  /** A texel's centre (world XY). */
  glm::vec2 centerOf(glm::ivec2 texel) const;
  /** The tile a texel is in. */
  static glm::ivec2 tileOf(glm::ivec2 texel);

  /** A texel's offset (0 where nothing's touched it). */
  float at(glm::ivec2 texel) const;
  void set(glm::ivec2 texel, float offset);
  /** The offset at a world XY point, bilinear between texel centres. */
  float sample(glm::vec2 point) const;

  bool empty() const { return m_tiles.empty(); }
  std::size_t tileCount() const { return m_tiles.size(); }
  /** The lowest offset in it (0 or below; kept as it's pressed, so after
   * relax() or clear() it may be lower than what's left). */
  float deepest() const { return m_deepest; }

  /**
   * Press texels down to their targets (those above them; deeper than
   * kDeepest is kept to it), pushing a share of the soil up alongside.
   * @return how much was pressed out (cubic metres).
   */
  float press(const Footprint& footprint, const PressSettings& settings = {});

  /**
   * Press a shape in, e.g. a footprint or a hoof: `size` across (world XY,
   * along its heading and across it), turned `heading` radians from the
   * world's +X, `depth` deep at its deepest, from the ground's offset at
   * `center` as it is. `shape`'s red (sampled smoothly; 1 deepest) is the
   * imprint, from above, its top along the heading; without one, a rounded
   * oval.
   */
  void stamp(glm::vec2 center,
             float heading,
             glm::vec2 size,
             float depth,
             const TextureData* shape = nullptr,
             const PressSettings& settings = {});

  /**
   * Weather it: each texel eased `smooth` (0..1) toward the average of its
   * neighbours (edges round off, ridges slump into ruts), then `fade`
   * (0..1) of what's left goes (the ground heals). Texels left all but flat
   * go.
   */
  void relax(float smooth, float fade = 0.0f);

  /**
   * Weather only some tiles (tile indices), each by its own amount: `corners`
   * (one for each tile) the smoothing at its four corners (x its lowest x and
   * y, then y the highest x, z the highest y, w both), blended across it, so
   * wet ground can weather while dry ground beside it stays as it is. No
   * fade. Only those tiles change (and are marked changed); tiles it doesn't
   * have are skipped.
   */
  void relax(std::span<const glm::ivec2> tiles,
             std::span<const glm::vec4> corners);

  /** Flatten a world XY rectangle (e.g. where a field's been tilled). */
  void clear(glm::vec2 low, glm::vec2 high);
  /** Flatten inside a world XY polygon (its corners in order). */
  void clear(std::span<const glm::vec2> polygon);
  /** Flatten everywhere. */
  void clearAll();

  /** The tiles changed since the last call (tile indices), and forget them.
   */
  std::vector<glm::ivec2> takeChanged();
  /** Whether anything's changed since takeChanged(). */
  bool changed() const { return !m_changed.empty(); }
  /**
   * The tiles changed since the last call to this (as takeChanged(), but
   * kept apart from it: for a game to follow what changes, while its module
   * uploads it), and forget them.
   */
  std::vector<glm::ivec2> takeRevised();
  /** Every tile it has (tile indices). */
  std::vector<glm::ivec2> tiles() const;
  /**
   * Whether it has any tile in a block of tiles, without listing them.
   * @param first the block's lowest tile, as a tile index
   * @param count how many tiles the block spans in x and y
   * @return true if one of its tiles is in the block
   */
  bool anyTileIn(glm::ivec2 first, glm::ivec2 count) const;

  /**
   * A tile's offsets as half floats (rows from its lowest y, kTile x kTile),
   * for the GPU; zeros for a tile it doesn't have.
   */
  void halves(glm::ivec2 tile, std::span<uint16_t> out) const;

  /**
   * The lowest offset over each texel of a coarser grid (`origin` its
   * corner, `texel` across, `size` texels; row-major, 0 where untouched):
   * as the puddles take it (on the GPU), so a rut narrower than their texels
   * still holds water.
   */
  std::vector<float>
  lowestOver(glm::vec2 origin, float texel, glm::ivec2 size) const;

  /**
   * Its offsets packed small, to keep with a saved game: only its tiles,
   * millimetres, runs of flat ground and small steps between neighbours in
   * few bytes. load() takes them back (false, leaving it empty, if they
   * aren't a map's).
   */
  std::vector<uint8_t> save() const;
  bool load(std::span<const uint8_t> bytes);

private:
  using Tile = std::vector<float>; // kTile * kTile, row-major from the lowest y
  static int64_t keyOf(glm::ivec2 tile);
  Tile* tileAt(glm::ivec2 tile);
  const Tile* tileAt(glm::ivec2 tile) const;
  Tile& makeTile(glm::ivec2 tile);
  // Texels read and written a run at a time (as press() does), the tile
  // last looked up, and last marked changed, remembered.
  struct Cursor
  {
    int64_t key = std::numeric_limits<int64_t>::min();
    Tile* tile = nullptr;
    int64_t changed = std::numeric_limits<int64_t>::min();
  };
  float at(glm::ivec2 texel, Cursor& cursor);
  void set(glm::ivec2 texel, float offset, Cursor& cursor);
  // press()'s second half: the soil pressed out pushed up alongside; and
  // around what was pressed (`first` to `last` in the footprint), walls too
  // steep slumped.
  void pushUp(const Footprint& footprint,
              const std::vector<uint8_t>& under,
              const std::vector<uint8_t>& pressed,
              glm::ivec2 first,
              glm::ivec2 last,
              float pressedOut,
              const PressSettings& settings);
  void slump(const Footprint& footprint,
             const std::vector<uint8_t>& under,
             glm::ivec2 first,
             glm::ivec2 last,
             const PressSettings& settings);
  // Tiles flat again (every texel within a hair of 0) go.
  void dropFlat(std::span<const glm::ivec2> tiles);

  float m_texel;
  float m_deepest = 0.0f;
  std::unordered_map<int64_t, Tile> m_tiles;
  std::unordered_set<int64_t> m_changed;
  std::unordered_set<int64_t> m_revised; // takeRevised()'s
  void markChanged(int64_t key);
};

/**
 * A terrain mesh's top surface as placed, to ask its height (and texture
 * coordinates) under any world XY point quickly: its triangles facing up,
 * bucketed over a coarse grid. (Built once for a mesh and its placement.)
 */
class GroundSurface
{
public:
  GroundSurface() = default;
  GroundSurface(std::span<const glm::vec3> positions,
                std::span<const uint32_t> indices,
                std::span<const glm::vec2> uvs,
                const glm::mat4& model);

  struct Point
  {
    float height = 0.0f;
    glm::vec2 uv{0.0f};
  };
  /** The highest of its surface over a point, if any. */
  std::optional<Point> at(glm::vec2 point) const;
  /** And which of its triangles that's on (for heightOn()). */
  std::optional<Point> at(glm::vec2 point, uint32_t& triangle) const;
  /**
   * The height of one of its triangles' plane over a point: at()'s there,
   * if it's on that triangle (as at() found it at points all round it).
   */
  float heightOn(uint32_t triangle, glm::vec2 point) const;

  bool empty() const { return m_triangles.empty(); }
  glm::vec2 low() const { return m_low; }
  glm::vec2 high() const { return m_high; }

  /**
   * Its surface over a grid (`origin` its corner, `texel` across, `size`
   * texels; row-major from the lowest y), each texel (height, u, v, 1), or
   * zeros off it: the patch's ground, for the GPU.
   */
  std::vector<glm::vec4>
  raster(glm::vec2 origin, float texel, glm::ivec2 size) const;

private:
  struct Triangle
  {
    glm::vec3 p[3];
    glm::vec2 uv[3];
  };
  std::vector<Triangle> m_triangles;
  glm::vec2 m_low{0.0f};
  glm::vec2 m_high{0.0f};
  float m_cell = 4.0f;
  glm::ivec2 m_cells{0};
  std::vector<std::vector<uint32_t>> m_buckets; // triangles over each cell
};

/**
 * The lowest point of a mesh's surface, as placed, over each texel of a
 * deformation map's grid, where it reaches below `below` (world Z): a
 * deformer's underside, seen from under the ground. NaN where nothing is.
 * Triangles entirely above `below` are skipped, so a tractor's body costs
 * nothing; its tyres' lugs and an implement's tines show texel by texel.
 * Each texel is looked at in sixteen places: its bottom is theirs averaged
 * (where there's any), its coverage how many of them the mesh is over, so
 * edges are smooth, not stepped.
 */
struct Underside
{
  glm::ivec2 from{0};
  glm::ivec2 size{0};
  std::vector<float> bottom;   // world Z
  std::vector<float> coverage; // 0 to 1

  bool empty() const { return size.x <= 0 || size.y <= 0; }
};

Underside underside(std::span<const glm::vec3> positions,
                    std::span<const uint32_t> indices,
                    const glm::mat4& model,
                    float below,
                    float texel = kDeformationTexel);

/**
 * A mesh made ready to find its underside often (a deformer's, each time it
 * moves): its triangles in small groups of neighbours, each group's bounds
 * in the mesh's own space, so underside() passes over the groups nowhere
 * near `below` (a tyre's top, a tractor's body) without placing them, and
 * which of them face up on a closed solid (one facing down is always under
 * those). The underside's the same as from the mesh itself, but for a hair
 * where a solid's cut by `below`. Made once for a mesh.
 */
class UndersideMesh
{
public:
  UndersideMesh() = default;
  UndersideMesh(std::span<const glm::vec3> positions,
                std::span<const uint32_t> indices);

  bool empty() const { return m_groups.empty(); }

private:
  friend Underside underside(const UndersideMesh& mesh,
                             const glm::mat4& model,
                             float below,
                             float texel);
  friend float lowestPoint(const UndersideMesh& mesh, const glm::mat4& model);
  struct Group
  {
    glm::vec3 center{0.0f}; // its bounding sphere's
    float radius = 0.0f;
    uint32_t firstVertex = 0, vertices = 0;
    uint32_t firstIndex = 0, indices = 0; // into its own vertices
  };
  std::vector<glm::vec3> m_vertices;
  std::vector<uint32_t> m_indices;
  // Each triangle's (as m_indices): on a closed solid, 1 if it goes round
  // anticlockwise seen from outside, -1 clockwise; 0 not on one.
  std::vector<int8_t> m_outwards;
  std::vector<Group> m_groups;
};

Underside underside(const UndersideMesh& mesh,
                    const glm::mat4& model,
                    float below,
                    float texel = kDeformationTexel);

/**
 * The lowest a mesh's points are placed (`model`): world Z, exactly (only
 * the groups that could reach lower than the lowest found so far are
 * placed). Unlike its bounds placed, the same however it's turned.
 */
float lowestPoint(const UndersideMesh& mesh, const glm::mat4& model);

/** Two undersides as one (the lowest of both over both). */
Underside merged(const Underside& a, const Underside& b);

/** An image's red at texture coordinates (clamped; 0, 0 its top left),
 * smooth, 0 to 1. */
float redAt(const TextureData& image, glm::vec2 uv);

/** A half float's bits (IEEE 754 binary16), as the GPU reads them. */
uint16_t toHalf(float value);

/**
 * A terrain material's deformation, as drawn (IRenderer::
 * setTerrainDeformation()): a window of its map around the camera, on the
 * GPU, which the terrain's patch (a fine mesh following the camera, drawn
 * with `patch`, a copy of its material) is displaced by, and the rest of
 * the ground is parallax-mapped with out to the window's edge.
 */
struct TerrainDeformation
{
  MaterialHandle patch;
  float texel = kDeformationTexel;
  // The window: `size` texels square (a multiple of DeformationMap::kTile),
  // wrapping (world texel t at t mod size), valid from world texel `from`.
  int size = 0;
  glm::ivec2 from{0};
  // The ground under the patch (GroundSurface::raster()), over the world XY
  // from `groundOrigin`, `groundTexel` across, `groundSize` texels.
  std::shared_ptr<const std::vector<glm::vec4>> ground;
  glm::vec2 groundOrigin{0.0f};
  float groundTexel = 0.25f;
  glm::ivec2 groundSize{0};
  // The patch: its centre (world XY), half its size, and the band inside its
  // edge where its displacement hands over to parallax.
  glm::vec2 patchCenter{0.0f};
  float patchHalf = 0.0f;
  float patchBand = 4.0f;
  float deepest = 0.0f; // the map's (DeformationMap::deepest())

  bool empty() const { return size <= 0; }
};

} // namespace sfs
