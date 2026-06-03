#pragma once

#include "engine/ecs/system.h"
#include "engine/particles/decal.h"
#include "engine/rendering/commands/commands.h"
#include "engine/rendering/isometricRenderContext.h" // IVec2Hash
#include "engine/rendering/renderProvider.h"
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

// Persistent terrain stains (blood, scorch, ...). Receives decals from collisions
// (IDecalSink), stores them chunked so they survive terrain streaming, and emits
// batched draw commands for the chunks near the camera (RenderProvider). Reuses
// the particle GL pipeline -- decals are unlit, depth-tested, alpha quads.
//
// The store is the source of truth (so decals are removable); a future
// optimisation can bake chunks into textures for extreme densities.
class DecalSystem
    : public System,
      public IDecalSink,
      public RenderProvider<IsometricRenderContext, ParticleBatchCommand>
{
public:
  // IDecalSink
  void addDecal(const DecalSpawn& spawn) override;

  // Removal. clearAll wipes everything; clearRegion wipes a tile rect (inclusive
  // min, exclusive max).
  void clearAll();
  void clearRegion(glm::ivec2 minTile, glm::ivec2 maxTile);

  std::size_t decalCount() const;

  // RenderProvider
  void computeCommands(const IsometricRenderContext& context) override;

protected:
  void update(double deltaTime) override;

private:
  struct Decal
  {
    glm::vec2 worldPos{0.0f, 0.0f};
    float elevation = 0.0f;
    DecalSurface surface = DecalSurface::Ground;
    uint8_t wallSide = 0;
    float wallBottom = 0.0f; // elevation of the wall face base (Wall only)
    float size = 0.15f;
    float rotation = 0.0f;
    glm::vec4 color{0.0f, 0.0f, 0.0f, 1.0f};
    const std::string* textureId = nullptr;
    float fadeRate = 0.0f;   // 0 = permanent; > 0 = linear alpha decay/sec
    float dripSpeed = 0.0f;  // wall drips: levels/sec the streak runs down
    float sortKey = 0.0f;    // wall drips: co-sort key of the host block
    float age = 0.0f;
    bool settled = false;    // wall drip finished running down -> now permanent
  };

  static constexpr int kChunkTiles = 16;

  glm::ivec2 chunkOf(const glm::vec2& worldPos) const;
  const std::string* internTexture(const std::string& id);

  void appendDecalQuad(const Decal& decal,
                       const IsometricRenderContext& context,
                       std::vector<ParticleQuad>& out) const;

  std::unordered_map<glm::ivec2, std::vector<Decal>, IVec2Hash> m_chunks;

  // Stable storage for texture-id strings, so the pointers stored on decals and
  // handed to render commands stay valid for the system's lifetime.
  std::unordered_set<std::string> m_textureIds;

  std::size_t m_fadingCount = 0;    // decals with fadeRate > 0 (water)
  std::size_t m_animatingCount = 0; // wall drips still running down (not settled)
};

} // namespace sfs
