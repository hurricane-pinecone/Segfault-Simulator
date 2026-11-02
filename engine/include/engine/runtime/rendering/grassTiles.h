#pragma once

#include "engine/core/rendering/grass.h"
#include "engine/runtime/rendering/framePlan.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace sfs
{

// Grass grown from tiles kept built (see GrassTileCache): a field's blades
// are worked out once per tile of its ground, from its ground and maps, and
// kept on the GPU; each frame, only what depends on the camera is done again,
// over the tiles in view.

/** A side of a grass tile, in its lattice's cells. */
inline constexpr int kGrassTileCells = 64;
/** Blades a tile's room comes in (and a frame's blades are grown in). */
inline constexpr uint32_t kGrassChunk = 64;
/** A kept blade (meshGrassGrow.wgsl's `KeptBlade`), in bytes. */
inline constexpr uint32_t kKeptBladeBytes = 16;

/**
 * GrassTileEntry::lattice's flags: for the debug view (SFS_GRASS_TILES), and
 * which end of a tile its blades are grown from first.
 */
inline constexpr uint32_t kGrassTileFresh = 4;     // built in the last moments
inline constexpr uint32_t kGrassTileEdited = 8;    // built again for an edit
inline constexpr uint32_t kGrassTileReversed = 16; // its high Y first

/**
 * `TileEntry` in meshGrassGrow.wgsl: a tile to build, or one to grow a
 * frame's blades from.
 */
struct GrassTileEntry
{
  glm::ivec2 origin{0, 0}; // its first cell, on its lattice
  uint32_t lattice = 0;    // which (0-3), and kGrassTile... flags
  uint32_t slot = 0;       // where the GPU keeps how many blades it has
  uint32_t first = 0;      // its first kept blade
  uint32_t room = 0;       // blades it has room for
  float threshold = 0.0f;  // built: those ranked below this are kept
  uint32_t firstChunk = 0; // grown from: its first of the frame's chunks
  glm::vec2 heights{0.0f}; // grown from: its ground's lowest and highest
  glm::vec2 unused{0.0f};
};
static_assert(sizeof(GrassTileEntry) == 48,
              "GrassTileEntry must match meshGrassGrow.wgsl's TileEntry");

/**
 * The ranks a lattice's blades have (see meshGrassGrow.wgsl): from x to y.
 * The finest's are the top 3/4, the next's the 3/16 below, and so on, the
 * coarsest's all the way down to 0.
 */
glm::vec2 grassLatticeRanks(int lattice);

/**
 * A tile keeps its lattice's blades ranked below a threshold, in steps (its
 * level) of half its lattice's top rank and half again: all of them at level
 * 0, fewer at each level up. The threshold of a level.
 */
float grassTileThreshold(int lattice, int level);

/**
 * The highest level whose threshold is at least `needed` (so a tile at it, or
 * below, keeps every blade ranked below `needed`).
 */
int grassTileLevel(int lattice, float needed);

/**
 * The room a tile of a lattice keeping blades ranked below `threshold` needs
 * (a multiple of kGrassChunk): about how many of its cells' blades rank
 * that low, with room to spare that's next to never short.
 */
uint32_t grassTileRoom(int lattice, float threshold);

/**
 * Room in a buffer, given out in chunks: first fit, and joined up again as
 * it's given back.
 */
class GrassChunkPool
{
public:
  explicit GrassChunkPool(uint32_t chunks = 0) { grow(chunks); }

  /** Room for `chunks`: the first of them; none if there isn't. */
  std::optional<uint32_t> allocate(uint32_t chunks);
  /** Give back what allocate() gave. */
  void free(uint32_t first, uint32_t chunks);
  /** More room, after what there is, to `chunks` in all. */
  void grow(uint32_t chunks);

  uint32_t size() const { return m_size; }
  uint32_t used() const { return m_used; }

private:
  void release(uint32_t first, uint32_t chunks); // into m_free, joined up

  std::map<uint32_t, uint32_t> m_free; // first, how many
  uint32_t m_size = 0;
  uint32_t m_used = 0;
};

/**
 * A grass field's tiles, kept built (on the GPU) as long as they're in use,
 * and the work of a frame's growth: which tiles to build, and which to grow
 * blades from.
 *
 * Its ground is split into fixed tiles (kGrassTileCells of each of its four
 * lattices' cells a side, from the ground's own origin). A tile keeps its
 * blades: where each is, how tall, its ground's slope, all that doesn't
 * depend on the camera. Each frame, the tiles near enough and in view are
 * grown from, and those not built yet are built first, from the same ground
 * and maps (so a tile coming into view looks just as it would have).
 *
 * Its far blades are mostly thinned out, so a tile keeps only those ranked
 * below what's needed nearest the camera (and a step more): built again
 * with more as the camera nears, and fewer when it's moved well away.
 *
 * Built again when what it's grown from changes: changed() where the maps or
 * the terrain under them did; all of them when the ground, the field's
 * placement or its blades' settings do. Tiles not grown from for a while,
 * or out of room, are let go (least lately used first).
 */
class GrassTileCache
{
public:
  /** Levels a tile can be built at (grassTileThreshold()). */
  static constexpr int kTileLevels = 21;

  struct Limits
  {
    uint32_t startChunks = 4096;   // room at first (256k blades)
    uint32_t softChunks = 32768;   // before letting go of unused tiles
    uint32_t mostChunks = 131072;  // the most room (8M blades, 128 MB)
    uint32_t mostTiles = 65536;    // tiles built at once
    uint64_t unusedFrames = 180;   // let go after this long unused
    uint32_t shrinksPerFrame = 16; // built again, fewer kept, a frame
    uint64_t freshFrames = 20;     // shown fresh (debug view)
  };

  GrassTileCache() = default;
  explicit GrassTileCache(const Limits& limits) : m_limits(limits) {}

  /** A new ground (all its tiles built again). */
  void setGround(const GrassHeights& heights);

  /**
   * What the blades grow from changed over a rectangle of the ground's own
   * XY: the tiles over it are built again (when they're next in use).
   */
  void changed(glm::vec2 min, glm::vec2 max);

  /** Every tile built again. */
  void clear();

  /** A frame's work. */
  struct Frame
  {
    std::vector<GrassTileEntry> builds; // tiles to build first
    std::vector<GrassTileEntry> drawn;  // tiles to grow from, nearest first
    uint32_t chunks = 0;                // all of theirs
    uint32_t outOfRoom = 0;             // tiles in use there's no room for
  };

  /**
   * Plan frame `frame`'s work, by its grow pass's settings (planGrass(): its
   * placement, the camera, the view, how far each lattice reaches and how
   * it thins out): the tiles near enough and in view, those not built (or
   * not with enough blades) to build. With `debug`, those built lately are
   * marked (kGrassTileFresh, kGrassTileEdited).
   */
  const Frame& plan(const GrassGrowUniforms& grow, uint64_t frame, bool debug);

  /** The room the GPU's buffer of kept blades needs (chunks). */
  uint32_t poolChunks() const { return m_pool.size(); }
  /** Of it, in use. */
  uint32_t usedChunks() const { return m_pool.used(); }
  /** Tiles built. */
  std::size_t tiles() const { return m_built; }
  /** Whether a tile is built (`lattice`, its cell `origin`). */
  bool built(int lattice, glm::ivec2 origin) const;

private:
  struct Tile
  {
    int lattice = 0;
    int index = -1;     // in its lattice's Grid (-1: none, a free slot)
    uint32_t first = 0; // chunks
    uint32_t chunks = 0;
    int level = 0;
    uint64_t built = 0; // the frame it was
    uint64_t used = 0;  // the last frame grown from
    bool edited = false;
  };
  // What the tiles were built for: if it changes, they're all built again.
  struct Built
  {
    glm::mat4 model{0.0f};
    glm::vec4 shape{0.0f}; // height, variation, -, cell
    glm::vec4 ground{0.0f};
    glm::ivec4 groundSize{0};
    glm::vec4 maps{0.0f};
    bool operator==(const Built&) const = default;
  };
  // A lattice's tiles over the ground: `size` a side from tile `from`
  // (`side` world units). For each, the ground's lowest and highest under
  // it (worked out when first needed: NaN till then; low above high: none),
  // the slot of the one built (-1: none), and whether it was let go for an
  // edit.
  struct Grid
  {
    glm::ivec2 from{0, 0};
    glm::ivec2 size{0, 0};
    float side = 0.0f;
    std::vector<glm::vec2> under;
    std::vector<int32_t> slot;
    std::vector<uint8_t> edited;
    int indexOf(glm::ivec2 tile) const; // -1 off it
  };

  // For the thinning they were worked out for (by the grow pass's settings):
  // from how far away each level of each lattice is enough, and is to be
  // built (a step more), and none are needed; each level's threshold, and
  // the room it needs (chunks).
  struct Levels
  {
    glm::vec4 thinning{-1.0f};
    float far = -1.0f;
    std::array<std::array<float, kTileLevels>, 4> from{};
    std::array<std::array<float, kTileLevels>, 4> buildFrom{};
    std::array<float, 4> none{};
    std::array<std::array<float, kTileLevels>, 4> thresholds{};
    std::array<std::array<uint32_t, kTileLevels>, 4> chunks{};
  };

  // The grids for cells `cell` across (none without a ground).
  void makeGrids(float cell);
  void workOutLevels(const GrassGrowUniforms& grow);
  // The level enough at `away`, by a lattice's `from` (Levels).
  static int
  levelAt(const std::array<float, kTileLevels>& from, int lattice, float away);
  // The lowest and highest ground under a tile (low above high: none).
  glm::vec2 heightsUnder(int lattice, glm::ivec2 tile, int index);
  // Room for a tile (letting go of others, or making more, if need be).
  std::optional<uint32_t> roomFor(uint32_t chunks, uint64_t frame);
  void drop(uint32_t slot);

  Limits m_limits;
  GrassHeights m_heights;
  glm::vec2 m_lowest{0.0f}; // its lowest and highest ground
  Built m_for;
  Levels m_levels;
  std::array<Grid, 4> m_grids;
  GrassChunkPool m_pool;
  std::vector<Tile> m_tiles; // by slot
  std::vector<uint32_t> m_freeSlots;
  std::size_t m_built = 0;
  Frame m_frame;
  // Sorting a frame's tiles grown from, nearest first: how near each is.
  std::vector<float> m_away;
  std::vector<uint32_t> m_order;
  std::vector<GrassTileEntry> m_sorted;
};

} // namespace sfs
