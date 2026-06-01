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
    // Share the Terrain pass so projected shadows interleave with blocks by
    // world depth (a block in front occludes the shadow). The per-command depth
    // is set from the receiver tile; subpass 0 plus that depth's +bias keeps the
    // shadow above the ground tile it lands on but below actors. A separate
    // Shadow pass would always draw after all terrain, painting over blocks.
    order = {RenderPass::Terrain, 0, 0};
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
