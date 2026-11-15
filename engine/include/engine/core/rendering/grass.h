#pragma once

#include "glm/glm/common.hpp"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include "glm/glm/ext/vector_int2.hpp"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace sfs
{

// Grass: the ground it grows on (GrassGround), the maps that say how it
// grows there (GrassMaps), and where it's flattened (GrassTrample). The GPU
// grows the blades themselves from these (see GrassModule and
// meshGrassGrow.wgsl), keeping them in tiles until what they grow from
// changes (GrassTileCache).

/** About how wide a grass grid's texel is, in world units. */
inline constexpr float kGrassTexel = 0.25f;

/** Most texels a grass grid has a side. */
inline constexpr int kGrassMaxTexels = 1024;

/** Least upward a surface faces to grow grass (cos 60 degrees). */
inline constexpr float kGrassMinUpness = 0.5f;

/**
 * A grid over a rectangle of a mesh's own XY: `size` texels from `min` to
 * `max`, rows from min y. Grass's ground and maps are grids over the mesh, so
 * they stay with it however the entity is moved or scaled.
 */
struct GrassGrid
{
  glm::vec2 min{0.0f};
  glm::vec2 max{0.0f};
  glm::ivec2 size{0, 0};

  /**
   * A grid over `min`..`max` with square texels about `texel` across (at
   * most `maxTexels` a side, so coarser for big rectangles).
   */
  static GrassGrid over(glm::vec2 min,
                        glm::vec2 max,
                        float texel,
                        int maxTexels = kGrassMaxTexels);

  bool empty() const { return size.x <= 0 || size.y <= 0; }
  std::size_t count() const
  {
    return empty() ? 0 : std::size_t(size.x) * size.y;
  }
  glm::vec2 texelSize() const { return (max - min) / glm::vec2(size); }
  /** The centre of texel (x, y). */
  glm::vec2 center(int x, int y) const
  {
    return min + (glm::vec2(x, y) + 0.5f) * texelSize();
  }
  /** Where a point is, in texels (texel (x, y)'s centre at x + 0.5, y + 0.5).
   */
  glm::vec2 toTexels(glm::vec2 point) const
  {
    return (point - min) / texelSize();
  }

  bool operator==(const GrassGrid&) const = default;
};

/**
 * A window of a big mesh's ground for grass to grow in (see GrassModule): a
 * square `half` either side of `center`, cut to the mesh's `min`..`max` (all
 * the mesh's own XY). Its texels are `texel` across, or twice, four times...
 * that, so it has at most kGrassMaxTexels a side; and they sit on multiples
 * of `texel` from the mesh's origin, wherever the window is. So a window's
 * ground meets another mesh's on the same lattice texel for texel (a hole in
 * one where the other is), and the same texels come back as it moves.
 * Empty if the square misses the mesh.
 */
GrassGrid grassWindow(glm::vec2 min,
                      glm::vec2 max,
                      glm::vec2 center,
                      float half,
                      float texel);

/**
 * How high a grass ground is (GrassGround), in blocks of its texels: the
 * lowest and highest ground in each, so the range under any rectangle is
 * quick to find (and small to keep).
 */
class GrassHeights
{
public:
  /** Texels a side of a block. */
  static constexpr int kBlock = 8;

  GrassHeights() = default;
  /** Of `texels` (xyz normal, zero where there's no ground; w height) over
   * `grid`. */
  GrassHeights(const GrassGrid& grid, std::span<const glm::vec4> texels);

  const GrassGrid& grid() const { return m_grid; }
  /**
   * The lowest and highest ground (the mesh's own Z) over texels `from` to
   * `to` (exclusive, clamped to the grid), near enough: over the blocks they
   * touch. Low above high if there's none.
   */
  glm::vec2 range(glm::ivec2 from, glm::ivec2 to) const;

private:
  GrassGrid m_grid;
  glm::ivec2 m_blocks{0, 0};
  std::vector<glm::vec2> m_ranges; // each block's (low above high: none)
};

/**
 * The ground grass grows on: a mesh seen from above, as a grid. Each texel
 * has the height (the mesh's own Z) and normal of the highest surface there
 * that faces up enough (when placed by the entity's transform), or nothing.
 */
class GrassGround
{
public:
  GrassGround() = default;

  /**
   * The ground of a mesh's triangles (`positions` indexed by `indices`, its
   * own space), placed by `model`, on a grid over its XY with texels about
   * kGrassTexel world units across. With `uvs` (one a position), each texel
   * keeps the mesh's texture coordinates there too (uv()). With `window`,
   * only the ground on that grid (e.g. a grassWindow()), and on it.
   */
  static GrassGround build(std::span<const glm::vec3> positions,
                           std::span<const uint32_t> indices,
                           const glm::mat4& model,
                           std::span<const glm::vec2> uvs = {},
                           const GrassGrid* window = nullptr);

  const GrassGrid& grid() const { return m_grid; }
  bool empty() const { return m_texels.empty(); }

  /** Whether texel (x, y) has ground. */
  bool has(int x, int y) const
  {
    return texel(x, y).x != 0.0f || texel(x, y).y != 0.0f ||
           texel(x, y).z != 0.0f;
  }
  /** Texel (x, y): its normal (the mesh's own space, unit) and height; zero
   * normal if none. */
  const glm::vec4& texel(int x, int y) const
  {
    return m_texels[std::size_t(y) * m_grid.size.x + x];
  }
  /** Every texel, rows from min y: as the GPU reads them. */
  std::span<const glm::vec4> texels() const { return m_texels; }

  /** How many texels have ground. */
  std::size_t groundTexels() const;

  /** How high its ground is, roughly, for finding it quickly. */
  const GrassHeights& heights() const { return m_heights; }

  /**
   * The mesh's texture coordinates at texel (x, y)'s ground (built with the
   * mesh's UVs; else none): where a terrain's splat map is there.
   */
  bool hasUvs() const { return !m_uvs.empty(); }
  glm::vec2 uv(int x, int y) const
  {
    return m_uvs.empty() ? glm::vec2(0.0f)
                         : m_uvs[std::size_t(y) * m_grid.size.x + x];
  }

private:
  GrassGrid m_grid;
  std::vector<glm::vec4> m_texels; // xyz normal, w height
  std::vector<glm::vec2> m_uvs;    // per texel, if built with UVs
  GrassHeights m_heights;
};

/** Texels a side of the tiles GrassMaps keeps its grids in. */
inline constexpr int kGrassMapTile = 16;

/**
 * A grid of values kept in square tiles (kGrassMapTile a side): a tile that's
 * one value all over keeps just that one, and only tiles where values differ
 * (where a road, a path, a mown edge or a field's edge crosses) keep a value
 * a texel. Setting a texel to something else splits its tile; compact() joins
 * it up again once it's all one value.
 */
template <class T>
class GrassTiles
{
public:
  GrassTiles() = default;
  GrassTiles(glm::ivec2 size, T value) : m_size(size)
  {
    m_tiles =
        (glm::max(size, glm::ivec2(0)) + kGrassMapTile - 1) / kGrassMapTile;
    m_values.assign(std::size_t(m_tiles.x) * m_tiles.y, value);
    m_texels.assign(m_values.size(), {});
  }

  glm::ivec2 size() const { return m_size; }
  /** Tiles across and down. */
  glm::ivec2 tiles() const { return m_tiles; }
  std::size_t tileCount() const { return m_values.size(); }
  /** The tile texel (x, y) is in. */
  int tileOf(int x, int y) const
  {
    return (y / kGrassMapTile) * m_tiles.x + x / kGrassMapTile;
  }

  T at(int x, int y) const
  {
    const int tile = tileOf(x, y);
    const std::vector<T>& texels = m_texels[tile];
    return texels.empty() ? m_values[tile] : texels[inTile(x, y)];
  }
  void set(int x, int y, const T& value)
  {
    const int tile = tileOf(x, y);
    std::vector<T>& texels = m_texels[tile];
    if (texels.empty())
    {
      if (m_values[tile] == value)
        return;
      texels.assign(std::size_t(kGrassMapTile) * kGrassMapTile, m_values[tile]);
    }
    texels[inTile(x, y)] = value;
  }
  /** Every texel `value` (all tiles one value). */
  void fill(const T& value)
  {
    std::fill(m_values.begin(), m_values.end(), value);
    for (std::vector<T>& texels : m_texels)
      std::vector<T>().swap(texels);
  }
  /** Whether a tile is one value all over (and so, uniform(), which). */
  bool isUniform(int tile) const { return m_texels[tile].empty(); }
  const T& uniform(int tile) const { return m_values[tile]; }
  /** A whole tile `value`. */
  void setTile(int tile, const T& value)
  {
    m_values[tile] = value;
    std::vector<T>().swap(m_texels[tile]);
  }
  /** A tile kept as one value again, if it's all one. */
  void compact(int tile)
  {
    std::vector<T>& texels = m_texels[tile];
    if (texels.empty())
      return;
    for (const T& value : texels)
      if (!(value == texels.front()))
        return;
    setTile(tile, T(texels.front()));
  }
  /** How many tiles keep a value a texel. */
  std::size_t mixedTiles() const
  {
    return std::size_t(std::count_if(m_texels.begin(),
                                     m_texels.end(),
                                     [](const std::vector<T>& texels)
                                     { return !texels.empty(); }));
  }
  /** About how much memory its values take (bytes). */
  std::size_t bytes() const
  {
    return m_values.size() * sizeof(T) +
           mixedTiles() * kGrassMapTile * kGrassMapTile * sizeof(T);
  }

private:
  static int inTile(int x, int y)
  {
    return (y % kGrassMapTile) * kGrassMapTile + x % kGrassMapTile;
  }

  glm::ivec2 m_size{0, 0};
  glm::ivec2 m_tiles{0, 0};
  std::vector<T> m_values;              // each tile's (uniform ones')
  std::vector<std::vector<T>> m_texels; // a mixed tile's (empty: uniform)
};

/**
 * How fast grass grows back toward what its maps say (GrassMaps::setRates()),
 * a day of growth (GrassMaps::setClock()): its coverage up and down, its
 * length up and down.
 */
struct GrassRates
{
  float coverUp = 0.2f;
  float coverDown = 0.5f;
  float lengthUp = 4.0f;
  float lengthDown = 0.5f;
};

/**
 * A mower's cut (GrassMaps::cut()): the strip its deck sweeps from `a` to `b`
 * (the mesh's own XY), `width` across, its edges square to it, leaving grass
 * no longer than `length` (of full length) that grows back at its rates'
 * class `lengthClass`. A mower going on along its way (`join`) cuts each
 * frame's strip on from the last, as one: its edges carry on unbroken.
 */
struct GrassCut
{
  glm::vec2 a{0.0f};
  glm::vec2 b{0.0f};
  float width = 1.0f;
  float length = 0.3f;
  uint8_t lengthClass = 0;
  bool join = false;
};

/**
 * Grass laid over the way something went (GrassMaps::roll()): a striping
 * roller behind a mower, or a mower's deck a little. The strip from `a` to `b`
 * (the mesh's own XY), `width` across, its blades leaning the way from `a` to
 * `b` by `strength` (0 to 1: 1 a roller's, as flat as rolled grass lies). As
 * the grass grows back it stands up again (GrassMaps::setLeanFade()). Only
 * mown grass keeps it (it's what's growing): rolling grass that isn't keeps
 * nothing.
 */
struct GrassRoll
{
  glm::vec2 a{0.0f};
  glm::vec2 b{0.0f};
  float width = 1.0f;
  float strength = 1.0f;
};

/**
 * A texel's grass as it was last changed (GrassMaps' growth): when (in the
 * maps' clock's ticks; kSettled: it's grown to what its maps say), how much
 * covered and how long it was then, how long it was laid flat to then (255:
 * not), and its rates' classes (coverage's low 4 bits, length's high 4).
 */
struct GrassAnchor
{
  static constexpr uint32_t kSettled = 0xffffffffu;
  uint32_t time = kSettled;
  uint8_t cover = 0;
  uint8_t length = 0;
  uint8_t flat = 255;
  uint8_t classes = 0;
  bool settled() const { return time == kSettled; }
  bool operator==(const GrassAnchor&) const = default;
};

/**
 * How grass grows over its ground, as grids the game and the editor's brush
 * write: how much grows (Coverage), how long it is (Length), four Data
 * channels the engine gives no meaning to, for a grass material to read
 * (moisture, say, for a game to colour dry grass yellow), and where it may
 * grow at all (Bounds).
 *
 * Grass gives out two ways. Where Coverage falls (worn or worked ground), it
 * thins and shortens over a band about kGrassEdgeBand texels into it, in
 * clumps (see grassEdgeBand()). Where Bounds falls below a half (a road, a
 * wall), it stops dead, to the blade: Bounds is read between texels and cut
 * at a half, so a game that writes it as a distance from the edge
 * (boundsValue()) gets an edge where the edge really is, finer than a texel,
 * with no band.
 *
 * Coverage and Length are what grass grows to. Set (set(), stamp(), fill())
 * it's that at once; with setTarget() it grows (or dies back) toward it,
 * and cut(), tear() and flatten() set it back: then each texel keeps when it
 * was last changed and how it was then (GrassAnchor), and how it is at any
 * time is worked out from that (now()): toward its target at its rates'
 * (setRates()), by the maps' clock (setClock(), days of growth: a game runs
 * it slower in a drought). Nothing's done a frame for grass that's growing;
 * the GPU's told how it is now only every so often, a tile at a time (by
 * update()), each time it's changed by a step it'd show.
 *
 * The maps keep their grids in tiles (GrassTiles): a tile that's all one
 * value keeps just that, so maps over wide ground cost little but where
 * something crosses them.
 *
 * Values are 0 to 1. Unpainted maps grow full, full-length grass everywhere
 * (coverage, length and bounds 1, data 0).
 */
class GrassMaps
{
public:
  enum Channel : uint8_t
  {
    Coverage,
    Length,
    Data0,
    Data1,
    Data2,
    Data3,
    Bounds,
  };
  static constexpr int kChannels = 7;

  /** How stamp() changes values under it. */
  enum class Blend : uint8_t
  {
    Toward, // move toward the value, by the brush's weight
    Add,    // add the value (negative to take away), times the weight
    Max,    // raise to the value (weighted), never lower
    Min,    // lower to the value (weighted), never raise
  };

  /** The default value of a channel. */
  static float defaultValue(Channel channel)
  {
    return channel <= Length || channel == Bounds ? 1.0f : 0.0f;
  }
  /** A channel's name as saved ("coverage", "length", "data0"...). */
  static std::string_view name(Channel channel);
  /** Whether the blades grow from a channel (Coverage, Length, Bounds). */
  static bool shapesBlades(Channel channel)
  {
    return channel <= Length || channel == Bounds;
  }
  /**
   * Bounds for a texel whose middle is `distance` from the edge grass grows
   * to (world units; below 0 on the side it's kept off), texels `texel`
   * across: a half at the edge, ramped over two texels either side. Take the
   * least of it and what's there to keep grass off several things at once.
   */
  static float boundsValue(float distance, float texel)
  {
    return std::clamp(0.5f + distance / (4.0f * texel), 0.0f, 1.0f);
  }

  /** Rates' classes (setRates()). */
  static constexpr int kRateClasses = 16;
  /** The maps' clock's ticks a day of growth. */
  static constexpr double kTicksPerDay = 1024.0;

  GrassMaps() = default;
  /** Maps over `grid`, every channel at its default. */
  explicit GrassMaps(const GrassGrid& grid);

  const GrassGrid& grid() const { return m_grid; }
  bool empty() const { return m_grid.empty(); }

  /** A channel at a point (the mesh's own XY), bilinear between texels; its
   * default off the grid. (Coverage and Length: what it grows to.) */
  float sample(Channel channel, glm::vec2 point) const;
  float at(Channel channel, int x, int y) const
  {
    return m_values[channel].at(x, y) / 255.0f;
  }
  /** Coverage or Length (else at()) as it is now, growing (see now()). */
  float sampleNow(Channel channel, glm::vec2 point) const;
  /** A channel's value now: Coverage and Length as grown by now. */
  float now(Channel channel, int x, int y) const;
  void set(Channel channel, int x, int y, float value);

  /**
   * Brush `value` into a channel over a circle (the mesh's own XY), `radius`
   * across: full `strength` (0 to 1) over the inner part, easing to nothing at
   * the edge (`softness` of the radius: 0 a hard edge, 1 soft all the way in).
   */
  void stamp(Channel channel,
             glm::vec2 center,
             float radius,
             float value,
             Blend blend = Blend::Toward,
             float strength = 1.0f,
             float softness = 0.5f);

  /** Every texel of a channel to `value`. */
  void fill(Channel channel, float value);

  // --- growth ----------------------------------------------------------

  /**
   * The maps' clock: how many days of growth there have been (any start; it
   * only ever goes on). Kept to 1/kTicksPerDay of a day, as whole ticks, so
   * it's exact however long a game runs (wrapping after some 11,000 years).
   */
  void setClock(double growthDays);
  uint32_t clock() const { return m_now; }
  /** A class of rates (0 to kRateClasses - 1; all GrassRates() at first). */
  void setRates(int rateClass, const GrassRates& rates);
  const GrassRates& rates(int rateClass) const;
  /** How fast grass laid flat (flatten()) stands up again, a day. */
  void setStandUp(float perDay) { m_standUp = std::max(perDay, 0.0f); }
  /**
   * How fast grass grows at a texel (x its rates' up; 1 at first, at most 2):
   * e.g. faster on wet ground. Kept as it is: change it only for grass
   * that's not growing, or its growth goes on as it was until it's next
   * changed.
   */
  void setGrowthScale(int x, int y, float scale);
  float growthScale(int x, int y) const
  {
    return m_growthScale.at(x, y) / 128.0f;
  }

  /**
   * Coverage or Length to `value` as what it grows to, from what it is now:
   * up or down at its rates (set() has it there at once).
   */
  void setTarget(Channel channel, int x, int y, float value);
  /** A texel's rates' classes, from now on (its growth goes on from now). */
  void setClasses(int x, int y, int coverClass, int lengthClass);
  int coverClass(int x, int y) const { return m_anchors.at(x, y).classes & 15; }
  int lengthClass(int x, int y) const
  {
    return m_anchors.at(x, y).classes >> 4;
  }
  /** Its coverage torn out (a rut): none now, growing back at `coverClass`. */
  void tear(int x, int y, int coverClass);
  /**
   * Grass laid flat over a circle: no longer than `to` (of full length) under
   * its inner part, easing to as it was at its edge (`softness` as stamp()'s),
   * standing up again at setStandUp()'s rate.
   */
  void flatten(glm::vec2 center, float radius, float to, float softness = 0.5f);
  /**
   * Mow a strip (see GrassCut): its grass no longer than the cut leaves it,
   * growing back from there. Its edges are kept finer than a texel (see
   * cutPixels()): the blades are cut to the line, at any angle.
   */
  void cut(const GrassCut& cut);
  /**
   * Lay a strip's grass over the way it was rolled (see GrassRoll): its
   * blades lean that way, standing up again as it grows. Its edges are
   * blended across a texel (as the lean's read between texels: a straight
   * edge stays straight).
   */
  void roll(const GrassRoll& roll);
  /**
   * How much grass grows back (of full length) for a lean laid on it to have
   * stood up again (0.25 at first).
   */
  void setLeanFade(float length) { m_leanFade = std::max(length, 0.01f); }
  /** Everything grown to its maps now (none growing). */
  void settle();
  /** Whether any of it's growing. */
  bool growing() const { return m_growingTiles > 0; }
  /** How many of its tiles are growing. */
  std::size_t growingTiles() const { return m_growingTiles; }

  /**
   * Its growth brought on to its clock, for the GPU: up to `mostTiles` of the
   * tiles growing that have changed by a step since they were last told of
   * (or have finished growing) are marked to be (takeTilesNow()).
   */
  void update(int mostTiles);
  /**
   * The tiles (indices, see GrassTiles) whose grass as it is now
   * (nowPixels(), cutPixels()) changed but for what takeChanged() tells of,
   * then forgets them.
   */
  std::vector<int> takeTilesNow();
  /**
   * Each tile: 1 if it or one round it is growing (its blades grown as they
   * are now, not as kept), else 0. With whether it changed since last asked.
   */
  const std::vector<uint8_t>& activeTiles() const { return m_active; }
  bool takeActiveChanged();
  glm::ivec2 tiles() const { return m_anchors.tiles(); }

  /**
   * The texels changed since the last call, as a rectangle (min inclusive, max
   * exclusive; empty if none), then forgets them.
   */
  std::pair<glm::ivec2, glm::ivec2> takeChanged();
  /**
   * Whether what changed (takeChanged()) took in Coverage, Length or Bounds
   * (else only data), then forgets it.
   */
  bool takeShapeChanged();
  /** Mark everything changed (e.g. for a fresh upload). */
  void markAllChanged();
  /** Texels a side of the blocks takeGrownChanged() tells of. */
  static constexpr int kGrownBlock = 16;
  /**
   * Where Coverage, Length or Bounds (what the blades are grown from)
   * changed since the last call, as rectangles of texels (xy min, zw max
   * exclusive): the kGrownBlock blocks with a change, run together along their
   * rows. Then forgets them. A change to a data channel isn't one of them.
   */
  std::vector<glm::ivec4> takeGrownChanged();

  /**
   * The GPU's two RGBA8 images of a rectangle of texels (rows from its min y):
   * coverage, length, 1 (for grassEdgeBand()), bounds; then data 0 to 3.
   * (Coverage and length: what it grows to.)
   */
  void pixels(glm::ivec2 from,
              glm::ivec2 to,
              std::vector<uint8_t>& shape,
              std::vector<uint8_t>& data) const;
  /**
   * The band where coverage gives out (grassEdgeBand(), the blue of pixels()'
   * shape image once worked out) over a rectangle, kept for nowPixels().
   */
  void keepBand(glm::ivec2 from, glm::ivec2 to, std::span<const uint8_t> shape);
  /**
   * How the grass is now, as the GPU grows it (RGBA8, rows from `from`'s y):
   * coverage and length now, the band (keepBand()) and bounds.
   */
  void
  nowPixels(glm::ivec2 from, glm::ivec2 to, std::vector<uint8_t>& now) const;
  /**
   * Where it's been cut (RGBA8): r the distance from a cut's edge, as bounds
   * are kept (boundsValue(): a half at the edge, above it inside the cut),
   * read between texels and cut at a half, so the blades either side take
   * their lengths only from the texels on their side; g and b the way its
   * blades lean (roll()), as it is now (x and y, 0.5 + 0.5 x, standing up as
   * it grows back); a how long the cut left it.
   */
  void
  cutPixels(glm::ivec2 from, glm::ivec2 to, std::vector<uint8_t>& cut) const;

  /** About how much memory it takes (bytes). */
  std::size_t bytes() const;

  /**
   * As saved: {"grid": {"min", "max", "size"}, "coverage": ..., "length": ...,
   * ...}, each channel as base64 run-length pairs (count, value), only those
   * not all at their default. Maps with no grid save as {}. (What grows to;
   * not how it's growing.)
   */
  nlohmann::json toJson() const;
  /** Maps read from toJson()'s form; none (empty) if it isn't one. */
  static GrassMaps fromJson(const nlohmann::json& json);

  bool operator==(const GrassMaps& other) const;

private:
  // How a texel is now: coverage and length, and how long it's laid flat
  // to (above 1: not).
  struct Grown
  {
    float cover = 1.0f;
    float length = 1.0f;
    float flat = 2.0f;
  };
  Grown grownAt(int x, int y) const;
  // How long (ticks) an anchor takes to grow to its targets.
  uint32_t ticksToSettle(const GrassAnchor& anchor,
                         float cover,
                         float length,
                         float scale) const;
  float fastestRate(const GrassAnchor& anchor, float scale) const;
  // A texel's growth set again from now (as it is now), `change` then
  // making it how it's to be.
  template <class Change>
  void reanchor(int x, int y, Change&& change);
  // A texel's anchor (and its tile's growth) as it now is.
  void putAnchor(int x, int y, const GrassAnchor& anchor);
  // A texel grown to its maps at once (set()).
  void snap(int x, int y);
  void growingChanged(int tile);
  void schedule(int tile);
  void markTileNow(int tile);
  void settleTile(int tile);
  bool settledRound(int tile) const; // it and those round it
  void touch(Channel channel, int x, int y);
  void markAllGrown();

  GrassGrid m_grid;
  std::array<GrassTiles<uint8_t>, kChannels> m_values;
  glm::ivec2 m_changedMin{0, 0};
  glm::ivec2 m_changedMax{0, 0};
  // Each kGrownBlock block: whether its Coverage or Length changed (all of
  // them, with m_allGrown).
  std::vector<uint8_t> m_grownBlocks;
  bool m_anyGrown = false;
  bool m_allGrown = false;
  bool m_shapeChanged = false;

  // Growth: each texel's anchor, how fast it grows, where it's been cut
  // and the band (keepBand()).
  GrassTiles<GrassAnchor> m_anchors;
  GrassTiles<uint8_t> m_growthScale;
  GrassTiles<uint8_t> m_cut;
  // Each texel's lean (roll()) and cut: x and y (int8: the way it leans, by
  // how much), how long it was when it was laid, how long the cut left it.
  GrassTiles<uint32_t> m_lean;
  float m_leanFade = 0.25f;
  GrassTiles<uint8_t> m_band;
  std::array<GrassRates, kRateClasses> m_rates{};
  float m_standUp = 4.0f;
  uint32_t m_now = 0;
  // Each tile: whether any of it's growing; when it's all grown (ticks);
  // the fastest any of it changes (a day); and when it's next to be told of
  // (kSettled: not), with those due, soonest first.
  std::vector<uint8_t> m_growingTile;
  std::vector<uint32_t> m_settleAt;
  std::vector<float> m_fastest;
  std::vector<uint32_t> m_dueAt;
  std::vector<std::pair<uint32_t, int>> m_due; // a heap: (when, tile)
  std::size_t m_growingTiles = 0;
  std::vector<uint8_t> m_active;
  bool m_activeChanged = true;
  std::vector<uint8_t> m_tileNow; // marked for takeTilesNow()
  std::vector<int> m_tilesNow;
};

/** How many texels into grass the band where coverage gives out reaches. */
inline constexpr int kGrassEdgeBand = 3;

/**
 * The band where grass gives out at a fall in coverage, into a rectangle of
 * GrassMaps::pixels()'s shape image (`size` texels, coverage as the blades
 * grow to it): each texel's blue becomes how far in it is, the least over the
 * texels round it of their coverage plus how many texels away they are over
 * kGrassEdgeBand (1: full grass at least that far round). The GPU grows grass
 * thinner and shorter, in clumps, where it's low (meshGrassGrow.wgsl's
 * grassShape()). A change of coverage moves it kGrassEdgeBand texels round;
 * a texel's is only right with the image reaching that much past it.
 */
void grassEdgeBand(std::span<uint8_t> shape, glm::ivec2 size);

/** What blades grow like (GrassComponent's settings for them). */
struct GrassShape
{
  float density =
      400.0f;           // blades per square world unit, where coverage is full
  float height = 0.35f; // full length, where the Length map is 1
  float heightVariation = 0.4f; // how much lengths vary (0: all the same)
  float width = 0.02f;          // at the root
};

/**
 * How grass grows by its vigour (GrassVigourField): each blade's vigour is
 * smoothstep(0, 1, w * scale + bias), w the field where it grows (`outside`
 * past it) plus a little smooth noise of the ground's (`detail` of it, in
 * patches about `detailSize` world units across). Where it's 0 grass is
 * `lengthLeast` of its length and `densityLeast` as dense; where it's 1, as
 * its maps say. With `drivesData0`, the material's Data 0 is 1 - vigour
 * (dryness, say) in place of the maps'. Off: as the maps say.
 */
struct GrassVigourSettings
{
  bool on = false;
  float scale = 1.0f;
  float bias = 0.0f;
  float lengthLeast = 1.0f;
  float densityLeast = 1.0f;
  float detail = 0.0f;
  float detailSize = 8.0f;
  float outside = 0.5f;
  bool drivesData0 = false;
  bool operator==(const GrassVigourSettings&) const = default;
};

/**
 * A field over the world (world XY, `size` values from `min` to `max`, rows
 * from min y) that grass grows by: how wet the ground is, say (see
 * GrassVigourSettings). Coarse (metres a value), read smoothly between them.
 */
struct GrassVigourField
{
  glm::vec2 min{0.0f};
  glm::vec2 max{0.0f};
  glm::ivec2 size{0, 0};
  std::vector<float> values;

  bool empty() const { return size.x <= 0 || size.y <= 0; }
  /** Bilinear at a world XY point; `outside` off it. */
  float sample(glm::vec2 point, float outside) const;
};

/** The ground's smooth detail noise vigour takes (GrassVigourSettings), 0..1.
 */
float grassVigourDetail(glm::vec2 point, float size);

/** A blade's vigour at a world point, as the GPU grows it (0..1). */
float grassVigour(const GrassVigourField* field,
                  const GrassVigourSettings& settings,
                  glm::vec2 point);

/** Something flattening grass: a sphere, and how it's moving. */
struct GrassBender
{
  glm::vec3 center{0.0f};
  float radius = 0.5f;
  glm::vec3 velocity{0.0f}; // world units a second
};

/**
 * How flattened grass is, and which way: a grid over its ground (world XY),
 * pressed where benders reach into the grass and springing back over time.
 * Its image() is what the GPU bends blades by.
 */
class GrassTrample
{
public:
  /** Texels a world unit, and most a side. */
  static constexpr float kTexelsPerUnit = 8.0f;
  static constexpr int kMaxTexels = 512;

  GrassTrample() = default;

  /** A map over `ground` placed by `model`, its blades up to `bladeHeight`
   * tall. */
  GrassTrample(const GrassGround& ground,
               const glm::mat4& model,
               float bladeHeight);

  /**
   * Spring back (fully over `recovery` seconds), then press down where
   * `benders` reach into the grass: grass under one lies flat, softening to
   * its edge. A moving one lays it down the way it's going (fanning out a
   * little at its sides), all along its path; a still one presses it outward.
   *
   * Only grass that's down, or that a bender reaches, is worked on: a still
   * bender, after it's first pressed its grass, changes nothing.
   *
   * @return whether its image() changed (where: takeChanged())
   */
  bool
  update(float seconds, std::span<const GrassBender> benders, float recovery);

  /** How flattened the grass at a world XY point is, 0 to 1. */
  float flattened(glm::vec2 point) const;
  /** Which way it's pressed there (unit, or zero). */
  glm::vec2 direction(glm::vec2 point) const;

  /**
   * RGBA8, rows from the map's min y: rg the direction (x 0.5 + 0.5), b how
   * flattened, a 255.
   */
  std::vector<uint8_t> image() const { return m_pixels; }
  /** image() of a rectangle of texels (rows from its min y). */
  void pixels(glm::ivec2 from, glm::ivec2 to, std::vector<uint8_t>& out) const;

  /**
   * The texels whose image() changed since the last call, as a rectangle (min
   * inclusive, max exclusive; empty if none), then forgets them. A new map's
   * are all of it.
   */
  std::pair<glm::ivec2, glm::ivec2> takeChanged();
  /** Mark everything changed (e.g. for a fresh upload). */
  void markAllChanged();

  glm::ivec2 size() const { return m_size; }
  glm::vec2 min() const { return m_min; }
  glm::vec2 max() const { return m_max; }

private:
  int indexOf(glm::vec2 point) const; // -1 off the map
  int tileOf(int x, int y) const { return (y / kTile) * m_tiles.x + x / kTile; }

  // Square tiles of texels, each marked if any of its grass is down (and, in
  // an update, if it's to be looked at for changes).
  static constexpr int kTile = 16;
  enum TileFlags : uint8_t
  {
    kDown = 1,
    kTouched = 2,
  };

  glm::vec2 m_min{0.0f};
  glm::vec2 m_max{0.0f};
  glm::ivec2 m_size{0, 0};
  float m_bladeHeight = 0.0f;
  std::vector<float> m_ground; // per texel: the ground's world height, or none
  std::vector<float> m_amount;
  std::vector<glm::vec2> m_direction;
  std::vector<uint8_t> m_pixels; // image(), kept up to date
  glm::ivec2 m_tiles{0, 0};
  std::vector<uint8_t> m_tileFlags;
  glm::ivec2 m_changedMin{0, 0};
  glm::ivec2 m_changedMax{0, 0};
};

} // namespace sfs
