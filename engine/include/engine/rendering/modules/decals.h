#pragma once

#include "engine/particles/decal.h"
#include "engine/rendering/commands/commands.h"
#include "engine/rendering/isometricRenderContext.h" // IVec2Hash
#include "engine/rendering/modules/renderModule.h"
#include "engine/rendering/vertices/vertices.h"
#include "glm/glm/ext/vector_float2.hpp"
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
class Decals
    : public IDecalSink,
      public CommandModule<DecalDrawCommand>
{
public:
  void addDecal(const DecalSpawn& spawn) override; // IDecalSink

  void clearAll();
  void clearRegion(glm::ivec2 minTile, glm::ivec2 maxTile);

  std::size_t decalCount() const;

  void update(double deltaTime) override;

  void computeCommands(const IsometricRenderContext& context) override;

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
    float sortKey = 0.0f; // wall co-sort key (host block); ground derives its own
    float age = 0.0f;
    bool settled = false;
  };

  struct ChunkData
  {
    std::vector<Decal> decals;
    // New static verts to append to the GPU buffer next frame (the common path:
    // a ground decal landed or a drip settled). O(new), not O(chunk total).
    std::vector<DecalVertex> pendingStatic;
    // A static decal was removed -> the GPU buffer must be rebuilt from scratch
    // (rare: clearRegion). Takes priority over pending appends.
    bool needsFullRebuild = false;
    int animatingCount = 0; // decals still animating (drips/fading water); when
                            // 0 the chunk needs no per-frame work
  };

  static constexpr int kChunkTiles = 16;

  glm::ivec2 chunkOf(const glm::vec2& worldPos) const;
  static std::int64_t chunkKey(glm::ivec2 chunk);

  const std::string* internTexture(const std::string& id);

  static bool isStatic(const Decal& d);

  // Build a decal's world-space vertices (ground square / wall streak + cap) into
  // `out`. Uses the cached tile size to size the wall cap to read round on screen.
  void buildDecalVerts(const Decal& decal, std::vector<DecalVertex>& out) const;

  std::unordered_map<glm::ivec2, ChunkData, IVec2Hash> m_chunks;
  std::unordered_set<std::string> m_textureIds; // stable id storage
  std::vector<std::int64_t> m_pendingFree;       // chunks freed since last frame

  // Cached from the projection each frame (constant in practice) so geometry can
  // be built at add/settle time, not only during computeCommands.
  float m_tileWidth = 32.0f;
  float m_elevationStep = 8.0f;

  std::size_t m_fadingCount = 0;    // fading (water) decals
  std::size_t m_animatingCount = 0; // running wall drips (not yet settled)
};

} // namespace sfs
