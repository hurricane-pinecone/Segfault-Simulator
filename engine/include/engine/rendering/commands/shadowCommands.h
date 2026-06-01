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
    order = {RenderPass::Shadow, 0, 1};
  }
};

struct SpriteShadowCommand : RenderCommand<FreeformQuad>
{
  const std::string* textureId = nullptr;

  SpriteShadowCommand()
  {
    // Drawn in the translucent Shadow pass, after all opaque depth is laid
    // down: the depth buffer (not painter interleaving) makes a block in front
    // occlude the shadow. Per-command depth is set from the receiver tile; its
    // small bias keeps the shadow just above the ground tile it lands on, while
    // nearer actors occlude it via depth-test.
    order = {RenderPass::Shadow, 0, 0};
    quad.tint = SDL_Color{0, 0, 0, 255};
  }
};

struct TerrainShadowBatchCommand : RenderCommand<QuadBatch>
{
  TerrainShadowBatchCommand() { order = {RenderPass::Shadow, 0.0f, 1}; }
};

using ShadowCommand = std::variant<TerrainShadowCommand,
                                   SpriteShadowCommand,
                                   TerrainShadowBatchCommand>;

} // namespace sfs
