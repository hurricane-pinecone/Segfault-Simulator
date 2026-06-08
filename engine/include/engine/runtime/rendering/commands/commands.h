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

  // Surface wave style, set per water source so 3D (voxel) and flat (ECS) water
  // can differ. waveStrength scales the Gerstner displacement (0 = a flat
  // plane); rippleStrength is the old flat colour-ripple animation (0 = off).
  float waveStrength = 1.0f;
  float rippleStrength = 0.025f;

  // The projection's linear basis: clip-space delta per unit of world-x,
  // world-y, and elevation. Lets the water vertex shader displace the surface
  // in world space (Gerstner waves) and re-project. Zero = no wave
  // displacement.
  glm::vec2 worldToClipX{0.0f, 0.0f};
  glm::vec2 worldToClipY{0.0f, 0.0f};
  glm::vec2 worldToClipE{0.0f, 0.0f};

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

// This frame's new permanent splats to stamp into one paint target's texture
// (a chunk's ground footprint or a single wall face). texW/texH size the target
// on first use; verts are in the target's [0,1] surface space.
struct DecalBakeBatch
{
  std::int64_t key = 0;
  int texW = 0;
  int texH = 0;
  std::vector<DecalBakeVertex> verts;
};

// A paint target's fixed draw geometry (the world-space quads that sample its
// paint texture: one per painted ground tile, or the single wall face). Sent
// only when the target's painted set changes, then kept on the GPU.
struct DecalDrawUpload
{
  std::int64_t key = 0;
  std::vector<DecalVertex> verts;
};

// Drives the baked decal pipeline for one frame. Permanent stains are baked
// into per-target paint textures (so a hammered spot costs the same as one
// hit); `bakes` stamps this frame's new splats, `drawUploads` refreshes a
// target's draw quads when its painted set grows, `drawKeys` draws every
// visible target, and `dynamic` carries the still-animating decals (fading
// water, running drips) that can't bake. Depth comes from the shader, so
// assignClipDepth skips this command.
struct DecalDrawCommand
{
  RenderOrder order{RenderPass::Decals, 0, 0};
  const std::string* textureId =
      nullptr; // decal sprite sampled by the bake pass + dynamic decals
  std::vector<DecalBakeBatch> bakes;
  std::vector<DecalDrawUpload> drawUploads;
  std::vector<std::int64_t> freeKeys; // paint targets to drop (cleared/emptied)
  std::vector<std::int64_t> drawKeys; // visible paint targets to draw
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
