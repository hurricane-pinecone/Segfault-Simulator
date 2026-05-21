#pragma once

#include "engine/renderers/commands/shadowCommands.h"
#include "engine/renderers/quads.h"
#include <variant>

namespace sfs
{

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

using AnyRenderCommand = std::variant<TexturedQuadCommand,
                                      FreeformQuadCommand,
                                      LitQuadCommand,
                                      LitQuadBatchCommand,
                                      TerrainShadowCommand,
                                      TerrainShadowBatchCommand,
                                      SpriteShadowCommand>;
} // namespace sfs
