#pragma once

#include "engine/core/components/surfaceEffect.h"
#include "engine/core/rendering/quads.h"
#include "engine/core/rendering/renderPass.h"
#include "engine/core/rendering/vertices.h"
#include "engine/core/types/blendMode.h"
#include "engine/runtime/rendering/commands/renderCommand.h"
#include "engine/runtime/rendering/commands/shadowCommands.h"
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace sfs
{

struct QuadCommand : RenderCommand<Quad>
{
};

struct TexturedQuadCommand : RenderCommand<TexturedQuad>
{
  const std::string* textureId = nullptr;
};

struct FreeformQuadCommand : RenderCommand<FreeformQuad>
{
  const std::string* textureId = nullptr;
};

struct LitQuadCommand : RenderCommand<LitQuad>
{
  const std::string* textureId = nullptr;
  const std::string* normalTextureId = nullptr;

  SurfaceEffect::Type type = SurfaceEffect::Type::None;
};

struct LitQuadBatchCommand : RenderCommand<LitQuadBatch>
{
  const std::string* textureId = nullptr;
  const std::string* normalTextureId = nullptr;

  SurfaceEffect::Type type = SurfaceEffect::Type::None;
};

struct SurfaceCommand
{
  std::vector<SurfaceVertex> vertices;
  std::vector<uint32_t> indices;

  float ambient = 1.0f;

  SurfaceEffect::Type type = SurfaceEffect::Type::None;
  RenderOrder order{RenderPass::Surfaces, 0, 0};
};

// A pre-batched set of particles sharing one texture + blend mode.
// assignClipDepth remaps each quad's z (set to its world sort-key) to
// clip-space depth, so the batch occludes against terrain per particle.
struct ParticleBatchCommand : RenderCommand<ParticleBatch>
{
  const std::string* textureId = nullptr;
  BlendMode blend = BlendMode::Alpha;
};

// (Re)upload of a decal chunk's persistent, world-space vertex buffer.
struct DecalChunkUpload
{
  std::int64_t key = 0;
  std::vector<DecalVertex> vertices;
};

// Drives the persistent decal pipeline for one frame. Carries only what changed
// (dirty-chunk uploads), the visible chunk keys to draw from the renderer's
// persistent buffers, and the small set of animating (running/fading) decal
// vertices rebuilt this frame. The bulk (settled decals) lives in the renderer.
// Depth comes from the shader, so assignClipDepth skips this command.
struct DecalDrawCommand
{
  RenderOrder order{RenderPass::Decals, 0, 0};
  const std::string* textureId =
      nullptr; // one texture for all decals (e.g. white_dot)
  std::vector<DecalChunkUpload>
      uploads; // chunks to fully (re)upload (after removal)
  std::vector<DecalChunkUpload>
      appends; // new static verts to append (the common path)
  std::vector<std::int64_t> freeKeys; // chunks to drop (cleared/emptied)
  std::vector<std::int64_t> drawKeys; // visible chunks to draw
  std::vector<DecalVertex> dynamic;   // animating decals, rebuilt this frame
};

// Opt-in real-geometry terrain: a lit, textured mesh of block faces (tops +
// exposed sides) sharing one material (texture + surface effect). Built per
// frame by the BlockGeometry module; assignClipDepth remaps each vertex's z
// (world key) to clip-space depth, like a SurfaceCommand mesh.
struct GeometryCommand
{
  RenderOrder order{RenderPass::Terrain, 0, 0};
  std::vector<GeometryVertex> vertices; // triangles (no index buffer)
  const std::string* textureId = nullptr;
  const std::string* normalTextureId =
      nullptr; // optional; geometry uses real normals
  SurfaceEffect::Type type = SurfaceEffect::Type::None;
};

using AnyRenderCommand = std::variant<QuadCommand,
                                      TexturedQuadCommand,
                                      FreeformQuadCommand,
                                      LitQuadCommand,
                                      LitQuadBatchCommand,
                                      TerrainShadowCommand,
                                      SurfaceCommand,
                                      TerrainShadowBatchCommand,
                                      SpriteShadowCommand,
                                      ParticleBatchCommand,
                                      DecalDrawCommand,
                                      GeometryCommand>;
} // namespace sfs
