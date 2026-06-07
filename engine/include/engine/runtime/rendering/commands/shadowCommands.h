#pragma once

#include "engine/core/rendering/quads.h"
#include "engine/runtime/rendering/commands/renderCommand.h"
#include <string>
#include <variant>

namespace sfs
{

struct TerrainShadowCommand : RenderCommand<Quad>
{
  TerrainShadowCommand()
  {
    quad.tint = {0, 0, 0, 255};
    order = {RenderPass::Shadow, 0, 1};
  }
};

struct SpriteShadowCommand : RenderCommand<FreeformQuad>
{
  const std::string* textureId = nullptr;

  SpriteShadowCommand()
  {
    // Drawn in the translucent SpriteShadow pass, after all opaque depth and
    // the terrain shadows. Its own pass keeps every sprite shadow contiguous so
    // they batch by texture instead of interleaving (by depth) with terrain
    // shadows. The depth buffer makes a block in front occlude the shadow;
    // per-command depth comes from the receiver tile.
    order = {RenderPass::SpriteShadow, 0, 0};
    quad.tint = {0, 0, 0, 255};
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
