#pragma once

#include "SDL2/SDL_pixels.h"
#include "engine/renderers/commands/renderCommand.h"
#include <variant>

namespace sfs
{

struct TerrainShadowCommand : RenderCommand<Quad>
{
  TerrainShadowCommand()
  {
    quad.tint = SDL_Color{0, 0, 0, 255};
    renderLayer = RenderLayer::Shadow;
  }
};

struct SpriteShadowCommand : RenderCommand<FreeformQuad>
{
  const std::string* textureId = nullptr;

  SpriteShadowCommand()
  {
    renderLayer = RenderLayer::Shadow;
    quad.tint = SDL_Color{0, 0, 0, 255};
  }
};

using ShadowCommand = std::variant<TerrainShadowCommand, SpriteShadowCommand>;

} // namespace sfs
