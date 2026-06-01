#pragma once

#include "engine/components/surfaceEffect.h"
#include "engine/rendering/commands/renderCommand.h"
#include "engine/rendering/commands/shadowCommands.h"
#include "engine/rendering/quads.h"
#include "engine/rendering/renderPass.h"
#include "engine/rendering/vertices/vertices.h"
#include <variant>

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

using AnyRenderCommand = std::variant<QuadCommand,
                                      TexturedQuadCommand,
                                      FreeformQuadCommand,
                                      LitQuadCommand,
                                      LitQuadBatchCommand,
                                      TerrainShadowCommand,
                                      SurfaceCommand,
                                      TerrainShadowBatchCommand,
                                      SpriteShadowCommand>;
} // namespace sfs
