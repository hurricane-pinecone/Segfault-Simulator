#pragma once

#include <memory>

#include <SDL.h>
#include <SDL_ttf.h>

namespace sfs
{

using TexturePtr = std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)>;

using SurfacePtr = std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)>;

using FontPtr = std::unique_ptr<TTF_Font, decltype(&TTF_CloseFont)>;

} // namespace sfs
