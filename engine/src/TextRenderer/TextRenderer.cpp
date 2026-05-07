#include "engine/logger/logger.h"
#include <SDL_hints.h>
#include <SDL_ttf.h>
#include <engine/TextRenderer/textRenderer.h>

namespace sfs
{

bool TextRenderer::m_initialized = false;
SDL_Renderer* TextRenderer::m_renderer = nullptr;
AssetStore* TextRenderer::m_assetStore = nullptr;

bool TextRenderer::init(SDL_Renderer& sdlRenderer, AssetStore& assetStore)
{
  m_renderer = &sdlRenderer;
  m_assetStore = &assetStore;

  if (TTF_Init() != 0)
  {
    LOG_ERROR(std::string("Failed to initialize SDL_ttf: ") + TTF_GetError());
    m_initialized = false;
    return false;
  }

  auto font = assetStore.addFont("default", "assets/fonts/m6x11.ttf", 24);
  if (!font)
  {
    return false;
  }
  TTF_SetFontHinting(font, TTF_HINTING_MONO);
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

  m_initialized = true;
  return true;
}

void TextRenderer::shutdown()
{
  if (m_initialized)
  {
    TTF_Quit();
  }

  m_initialized = false;
  m_renderer = nullptr;
  m_assetStore = nullptr;
}

bool TextRenderer::isInitialized() { return m_initialized; }

void TextRenderer::drawText(float x, float y, const std::string& text)
{
  SDL_Color white = {255, 255, 255, 255};
  drawText(x, y, text, "default", white);
}

void TextRenderer::drawText(float x,
                            float y,
                            const std::string& text,
                            const std::string& fontId)
{
  SDL_Color white = {255, 255, 255, 255};
  drawText(x, y, text, fontId, white);
}

void TextRenderer::drawText(float x,
                            float y,
                            const std::string& text,
                            const std::string& fontId,
                            SDL_Color color)
{
  if (!m_initialized)
  {
    return;
  }

  if (!m_renderer || !m_assetStore)
  {
    return;
  }

  if (text.empty())
  {
    return;
  }

  TTF_Font* font = m_assetStore->getFont(fontId);

  if (!font)
  {
    return;
  }

  SDL_Surface* surface = TTF_RenderText_Blended(font, text.c_str(), color);

  if (!surface)
  {
    LOG_ERROR(std::string("Failed to render text surface: ") + TTF_GetError());
    return;
  }

  SDL_Texture* texture = SDL_CreateTextureFromSurface(m_renderer, surface);

  if (!texture)
  {
    LOG_ERROR(std::string("Failed to create text texture: ") + SDL_GetError());
    SDL_FreeSurface(surface);
    return;
  }

  SDL_Rect dstRect;
  dstRect.x = static_cast<int>(x);
  dstRect.y = static_cast<int>(y);
  dstRect.w = surface->w;
  dstRect.h = surface->h;

  SDL_FreeSurface(surface);

  SDL_RenderCopy(m_renderer, texture, nullptr, &dstRect);

  SDL_DestroyTexture(texture);
}

} // namespace sfs
