#pragma once

#include "engine/core/particles/decal.h"
#include "engine/core/rendering/vertices.h"
#include "engine/runtime/rendering/commands/commands.h"
#include "engine/runtime/rendering/isometricRenderContext.h" // IVec2Hash
#include "engine/runtime/rendering/modules/renderModule.h"
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

// Render module for persistent terrain stains (blood, ...). Permanent decals
// are BAKED: each splat is stamped once into a paint texture (a chunk's
// world-XY footprint for ground, a single face for walls) and its geometry is
// thrown away. A hammered spot then costs the same as one hit, so memory is
// bounded by painted area, not spray count -- the texture is allocated once per
// painted chunk/face. Only still-animating decals (fading water, running wall
// drips) keep per-frame geometry, drawn from a small dynamic buffer until they
// settle (a settled drip bakes into its face).
//
// Emits one DecalDrawCommand/frame: new splats to bake, draw-quad refreshes for
// targets whose painted set grew, the visible target keys to draw, the
// animating vertices, and targets to free. The renderer holds the textures +
// buffers.
class IsometricDecalSink
    : public IDecalSink,
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
    float wallTop = 0.0f;
    // size.x = length (along rotation's local +X), size.y = width (across).
    glm::vec2 size{0.15f, 0.15f};
    float rotation = 0.0f;
    glm::vec4 color{0.0f, 0.0f, 0.0f, 1.0f};
    const std::string* textureId = nullptr;
    float fadeRate = 0.0f;
    float dripSpeed = 0.0f;
    // Sample the texture's solid centre (crisp streak) vs the full sprite (soft
    // blob). Lets a single dot texture read as both.
    bool crisp = false;
    float age = 0.0f;
    bool settled = false;
  };

  // A baked wall face (one tile edge, wallBottom..wallTop): its paint texture
  // size, the single quad that draws it, and any splats waiting to bake in.
  struct WallFace
  {
    std::int64_t targetId = 0; // renderer paint-target id (0 = unassigned)
    int texW = 0;
    int texH = 0;
    std::vector<DecalVertex> drawQuad;        // the face quad (built once)
    std::vector<DecalBakeVertex> pendingBake; // splats to bake this frame
    bool drawDirty = false;                   // draw quad needs (re)upload
  };

  struct ChunkData
  {
    // Animating decals only (fading water, running drips). Settled/baked decals
    // are not kept here -- they live in the paint textures.
    std::vector<Decal> decals;
    int animatingCount = 0;

    // --- Ground paint (one texture per chunk's world-XY footprint) ---
    std::int64_t groundTargetId = 0; // renderer paint-target id (0 = none yet)
    int groundTexW = 0;
    int groundTexH = 0;
    std::unordered_set<int> paintedTiles;     // tile index (ly*kChunkTiles+lx)
    std::vector<DecalVertex> groundDrawQuads; // one quad per painted tile
    std::vector<DecalBakeVertex> pendingGroundBake;
    bool groundDrawDirty = false;

    // --- Wall paint (one texture per painted face) ---
    std::unordered_map<std::int64_t, WallFace> wallFaces;
  };

  static constexpr int kChunkTiles = 16;
  // Paint resolution: texels per world tile. A chunk's ground texture is
  // kChunkTiles * kTexelsPerTile square; a wall face is one tile wide.
  static constexpr int kTexelsPerTile = 16;
  // Cap a very tall wall face's texture so a freak elevation can't allocate an
  // enormous texture.
  static constexpr int kMaxFaceElevTexels = 1024;

  glm::ivec2 chunkOf(const glm::vec2& worldPos) const;

  const std::string* internTexture(const std::string& id);

  // Static = bakeable now: not fading, and not a wall drip still running down.
  static bool isStatic(const Decal& d);

  // Key a wall decal to its face (tile edge + fixed coordinate + side).
  static std::int64_t faceKey(const Decal& d);

  // Bake a permanent decal: mark its tile/face painted (building the draw quad
  // on first paint) and queue its splat geometry to stamp into the target
  // texture. A repeat on the same spot just re-stamps (premultiplied paint
  // saturates); a different colour bakes over -- neither grows memory.
  void bakePermanent(glm::ivec2 chunkCoord, ChunkData& chunk, const Decal& d);

  // Queue every paint target a chunk owns for release (on clear).
  void queueChunkFree(const ChunkData& chunk);

  // --- Geometry builders -----------------------------------------------------
  // Ground splat -> chunk-local [0,1] bake quads (clipped to its tile).
  void buildGroundBake(const Decal& d,
                       glm::vec2 chunkOrigin,
                       std::vector<DecalBakeVertex>& out) const;
  // One ground tile's world-space draw quad sampling its paint sub-rect.
  void buildGroundTileDraw(int lx,
                           int ly,
                           glm::vec2 chunkOrigin,
                           float elevation,
                           std::vector<DecalVertex>& out) const;
  // Wall splat (impact mark or settled drip) -> face-local [0,1] bake quads.
  void buildWallBake(const Decal& d, std::vector<DecalBakeVertex>& out) const;
  // A wall face's world-space draw quad sampling its paint texture.
  void buildWallFaceDraw(const Decal& d, std::vector<DecalVertex>& out) const;

  // Build an animating decal's world-space vertices (fading water square / a
  // running wall drip streak + cap) into `out`, projected by the decal shader.
  void buildDecalVerts(const Decal& decal, std::vector<DecalVertex>& out) const;

  std::unordered_map<glm::ivec2, ChunkData, IVec2Hash> m_chunks;
  std::unordered_set<std::string> m_textureIds; // stable id storage
  const std::string* m_spriteId = nullptr; // decal sprite for the bake pass
  std::vector<std::int64_t> m_pendingFree; // targets freed since last frame
  std::int64_t m_nextTargetId = 1;         // monotonic paint-target ids

  // Cached from the projection each frame (constant in practice) so geometry
  // can be built at add/settle time, not only during computeCommands.
  float m_tileWidth = 32.0f;
  float m_elevationStep = 8.0f;

  bool m_visible = true; // debug toggle: emit draws at all

  std::size_t m_fadingCount = 0;    // fading (water) decals
  std::size_t m_animatingCount = 0; // running wall drips (not yet settled)
};

} // namespace sfs
