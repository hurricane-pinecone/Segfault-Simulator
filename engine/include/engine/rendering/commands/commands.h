#pragma once

#include "engine/rendering/commands/renderCommand.h"
#include "engine/rendering/commands/shadowCommands.h"
#include "engine/rendering/quads.h"
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
};

struct LitQuadBatchCommand : RenderCommand<LitQuadBatch>
{
  const std::string* textureId = nullptr;
  const std::string* normalTextureId = nullptr;
};

using AnyRenderCommand = std::variant<QuadCommand,
                                      TexturedQuadCommand,
                                      FreeformQuadCommand,
                                      LitQuadCommand,
                                      LitQuadBatchCommand,
                                      TerrainShadowCommand,
                                      TerrainShadowBatchCommand,
                                      SpriteShadowCommand>;
} // namespace sfs
