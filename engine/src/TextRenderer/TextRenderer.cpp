#include "engine/Color/Color.h"
#include "engine/logger/logger.h"
#include "engine/types/SDLPtrs.h"
#include <SDL_hints.h>
#include <SDL_ttf.h>
#include <engine/TextRenderer/textRenderer.h>

namespace sfs
{

bool TextRenderer::m_initialized = false;
SDL_Renderer* TextRenderer::m_renderer = nullptr;
AssetStore* TextRenderer::m_assetStore = nullptr;

std::unordered_map<std::string, CachedText> TextRenderer::m_textCache;

std::string TextRenderer::buildCacheKey(const std::string& text,
                                        const std::string& fontId,
                                        Color color)
{
  return fontId + "_" + std::to_string(color.r) + "_" +
         std::to_string(color.g) + "_" + std::to_string(color.b) + "_" +
         std::to_string(color.a) + "_" + text;
}

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

void TextRenderer::drawText(float x,
                            float y,
                            const std::string& text,
                            Color color)
{
  drawText(x, y, text, "default", color);
}

void TextRenderer::drawText(float x,
                            float y,
                            const std::string& text,
                            const std::string& fontId,
                            Color color)
{
  if (!m_initialized)
    return;

  if (!m_renderer || !m_assetStore)
    return;

  if (text.empty())
    return;

  TTF_Font* font = m_assetStore->getFont(fontId);

  if (!font)
    return;

  std::string cacheKey = buildCacheKey(text, fontId, color);

  auto it = m_textCache.find(cacheKey);

  if (it == m_textCache.end())
  {
    SDL_Surface* surface =
        TTF_RenderText_Blended(font, text.c_str(), color.toSDL());

    if (!surface)
    {
      LOG_ERROR(std::string("Failed to render text surface: ") +
                TTF_GetError());
      return;
    }

    SDL_Texture* texture = SDL_CreateTextureFromSurface(m_renderer, surface);

    if (!texture)
    {
      LOG_ERROR(std::string("Failed to create text texture: ") +
                SDL_GetError());

      SDL_FreeSurface(surface);
      return;
    }

    CachedText cached{
        surface->w, surface->h, TexturePtr(texture, SDL_DestroyTexture)};

    SDL_FreeSurface(surface);

    auto [insertedIt, inserted] =
        m_textCache.emplace(cacheKey, std::move(cached));

    it = insertedIt;
  }

  SDL_Rect dstRect;
  dstRect.x = static_cast<int>(x);
  dstRect.y = static_cast<int>(y);
  dstRect.w = it->second.width;
  dstRect.h = it->second.height;

  SDL_RenderCopy(m_renderer, it->second.texture.get(), nullptr, &dstRect);
}

} // namespace sfs
