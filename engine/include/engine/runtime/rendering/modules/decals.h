#pragma once

#include "engine/core/particles/decal.h"
#include "engine/runtime/rendering/commands/commands.h"
#include "engine/runtime/rendering/isometricRenderContext.h" // IVec2Hash
#include "engine/runtime/rendering/modules/renderModule.h"
#include "engine/runtime/rendering/vertices/vertices.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include "glm/glm/ext/vector_int2.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sfs
{

// Render module for persistent terrain stains (blood, ...). Decals are stored
// chunked in WORLD space; settled ones live in the renderer's persistent
// per-chunk GPU buffers and are projected on the GPU, so they cost nothing to
// re-derive as the camera moves. Only the few animating decals (running wall
// drips, fading water) are rebuilt per frame into a small dynamic buffer.
//
// Emits one DecalDrawCommand/frame: dirty-chunk uploads + visible chunk keys +
// the animating vertices + chunks to free. The renderer holds the heavy data.
class Decals : public IDecalSink,
               public CommandModule<IsometricRenderContext, DecalDrawCommand>
{
public:
  void addDecal(const DecalSpawn& spawn) override; // IDecalSink

  void clearAll();
  void clearRegion(glm::ivec2 minTile, glm::ivec2 maxTile);

  // Debug toggle: when hidden the module keeps accumulating stains but emits no
  // draw, so toggling preserves them (vs removing the module, which drops
  // them).
  bool visible() const { return m_visible; }
  void setVisible(bool visible) { m_visible = visible; }

  std::size_t decalCount() const;

  void update(double deltaTime) override;

  void computeCommands(const IsometricRenderContext& context) override;

  std::vector<ModuleSetting> settings(const IsometricRenderContext&) override
  {
    return {
        settings::text(
            "Decals", [this] { return std::to_string(decalCount()); }),
        settings::floatRange(
            "Ground per cell",
            1.0f,
            16.0f,
            [this] { return static_cast<float>(m_maxDecalsPerCell); },
            [this](float v)
            { m_maxDecalsPerCell = static_cast<int>(v + 0.5f); }),
        settings::floatRange(
            "Wall per cell",
            1.0f,
            16.0f,
            [this] { return static_cast<float>(m_maxWallDecalsPerCell); },
            [this](float v)
            { m_maxWallDecalsPerCell = static_cast<int>(v + 0.5f); }),
        settings::floatRange(
            "Cell size (tiles)",
            0.03f,
            0.5f,
            [this] { return m_coverageCell; },
            [this](float v)
            {
              m_coverageCell = v;
              rebuildAllCoverage();
            }),
        settings::floatRange(
            "Wall band (levels)",
            0.25f,
            4.0f,
            [this] { return m_coverageElevCell; },
            [this](float v)
            {
              m_coverageElevCell = v;
              rebuildAllCoverage();
            }),
        settings::action("Clear decals", [this] { clearAll(); }),
    };
  }

private:
  struct Decal
  {
    glm::vec2 worldPos{0.0f, 0.0f};
    float elevation = 0.0f;
    DecalSurface surface = DecalSurface::Ground;
    uint8_t wallSide = 0;
    float wallBottom = 0.0f;
    float size = 0.15f;
    float rotation = 0.0f;
    glm::vec4 color{0.0f, 0.0f, 0.0f, 1.0f};
    const std::string* textureId = nullptr;
    float fadeRate = 0.0f;
    float dripSpeed = 0.0f;
    float age = 0.0f;
    bool settled = false;
  };

  // Per-cell coverage record: how many permanent decals occupy the cell and the
  // paint colour that owns it (for repaint detection).
  struct CoverageCell
  {
    std::uint16_t count = 0;
    glm::vec3 color{0.0f};
  };

  struct ChunkData
  {
    std::vector<Decal> decals;
    // New static verts to append to the GPU buffer next frame (the common path:
    // a ground decal landed or a drip settled). O(new), not O(chunk total).
    std::vector<DecalVertex> pendingStatic;
    // A static decal was removed -> the GPU buffer must be rebuilt from scratch
    // (rare: clearRegion / a cell repainted a different colour). Takes priority
    // over pending appends.
    bool needsFullRebuild = false;
    int animatingCount = 0; // decals still animating (drips/fading water); when
                            // 0 the chunk needs no per-frame work
    // Coverage per small world cell (see coverageKey). Bounds memory by painted
    // AREA: once a cell holds its colour's quota, more of the same colour is
    // dropped; a different colour replaces it instead.
    std::unordered_map<std::int64_t, CoverageCell> coverage;
  };

  static constexpr int kChunkTiles = 16;

  // Spatial saturation: permanent decals (anything that doesn't fade -- ground,
  // walls, permanent water) are bucketed into small world cells. Once a cell
  // holds its colour's quota, more of THAT colour is dropped (it's already
  // painted) -- but a different colour replaces it, so you can paint over. A
  // face fills to roughly one layer of paint and stays there; memory tracks
  // painted area, not spray count. Fading decals don't count.
  // (Tuning lives in member variables below, exposed live on the debug panel.)
  // Cosine of the colour-direction angle below which two paints count as
  // different (so an effect's light->dark gradient is one colour, but red vs
  // blue is a repaint). Hue-based, so brightness doesn't trigger false
  // repaints.
  static constexpr float kColorSimilarity = 0.85f;

  glm::ivec2 chunkOf(const glm::vec2& worldPos) const;
  static std::int64_t chunkKey(glm::ivec2 chunk);

  const std::string* internTexture(const std::string& id);

  static bool isStatic(const Decal& d);

  // True if two paint colours are the "same paint" (similar hue direction),
  // ignoring brightness.
  static bool sameColor(const glm::vec3& a, const glm::vec3& b);

  // Key a decal to its coverage cell (chunk-local position + elevation band +
  // surface/side), so ground and wall paint saturate independently.
  std::int64_t coverageKey(glm::ivec2 chunk, const Decal& d) const;

  // Remove every permanent decal in a cell from the chunk (so a new colour can
  // replace it), flagging a GPU rebuild.
  void clearCell(glm::ivec2 chunk, ChunkData& data, std::int64_t key);

  // Rebuild a chunk's coverage from its current permanent decals (after a
  // removal, since coverage is otherwise only updated incrementally).
  void rebuildCoverage(glm::ivec2 chunk, ChunkData& data) const;

  // Re-key every chunk's coverage from its decals; called when the cell size or
  // elevation band changes (the debug sliders) so saturation re-accounts.
  void rebuildAllCoverage();

  // Build a decal's world-space vertices (ground square / wall streak + cap)
  // into `out`. Uses the cached tile size to size the wall cap to read round on
  // screen.
  void buildDecalVerts(const Decal& decal, std::vector<DecalVertex>& out) const;

  std::unordered_map<glm::ivec2, ChunkData, IVec2Hash> m_chunks;
  std::unordered_set<std::string> m_textureIds; // stable id storage
  std::vector<std::int64_t> m_pendingFree;      // chunks freed since last frame

  // Cached from the projection each frame (constant in practice) so geometry
  // can be built at add/settle time, not only during computeCommands.
  float m_tileWidth = 32.0f;
  float m_elevationStep = 8.0f;

  // Coverage tuning (live via the debug panel). A cell holds up to its
  // surface's quota of one paint colour, then drops more of that colour. Walls
  // get a lower quota because each wall decal is a tall streak covering far
  // more surface than a ground dot. The elevation band only affects walls
  // (ground sits at one elevation per tile): a larger band keeps fewer drips
  // down a face.
  bool m_visible = true;           // debug toggle: emit draws at all
  float m_coverageCell = 0.06f;    // world tiles
  float m_coverageElevCell = 1.0f; // elevation levels per band (walls)
  int m_maxDecalsPerCell = 16;     // ground / default quota
  int m_maxWallDecalsPerCell = 3;  // wall-face quota

  std::size_t m_fadingCount = 0;    // fading (water) decals
  std::size_t m_animatingCount = 0; // running wall drips (not yet settled)
};

} // namespace sfs
