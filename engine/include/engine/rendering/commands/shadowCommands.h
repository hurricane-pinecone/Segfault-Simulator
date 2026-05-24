#pragma once

#include "SDL2/SDL_pixels.h"
#include "engine/rendering/commands/renderCommand.h"
#include "engine/rendering/quads.h"
#include <variant>

namespace sfs
{

struct TerrainShadowCommand : RenderCommand<Quad>
{
  TerrainShadowCommand()
  {
    quad.tint = SDL_Color{0, 0, 0, 255};
    order = {RenderPass::Terrain, 0, 1};
  }
};

struct SpriteShadowCommand : RenderCommand<FreeformQuad>
{
  const std::string* textureId = nullptr;

  SpriteShadowCommand()
  {
    order = {RenderPass::Shadow, 0, 0};
    quad.tint = SDL_Color{0, 0, 0, 255};
  }
};

struct TerrainShadowBatchCommand : RenderCommand<QuadBatch>
{
  TerrainShadowBatchCommand() { order = {RenderPass::Terrain, 0.0f, 1}; }
};

using ShadowCommand = std::variant<TerrainShadowCommand,
                                   SpriteShadowCommand,
                                   TerrainShadowBatchCommand>;

} // namespace sfs
