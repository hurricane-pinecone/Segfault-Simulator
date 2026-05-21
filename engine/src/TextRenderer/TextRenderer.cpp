#include "engine/Color/Color.h"
#include "engine/logger/logger.h"
#include "engine/renderers/openGLQuadRenderer.h"
#include "engine/renderers/quads.h"
#include <SDL_hints.h>
#include <SDL_ttf.h>
#include <engine/TextRenderer/textRenderer.h>

namespace sfs
{

bool TextRenderer::m_initialized = false;
AssetStore* TextRenderer::m_assetStore = nullptr;
OpenGLQuadRenderer* TextRenderer::m_quadRenderer = nullptr;

std::unordered_map<std::string, CachedText> TextRenderer::m_textCache;

std::string TextRenderer::buildCacheKey(const std::string& text,
                                        const std::string& fontId,
                                        Color color)
{
  return fontId + "_" + std::to_string(color.r) + "_" +
         std::to_string(color.g) + "_" + std::to_string(color.b) + "_" +
         std::to_string(color.a) + "_" + text;
}

bool TextRenderer::init(OpenGLQuadRenderer& quadRenderer,
                        AssetStore& assetStore)
{
  m_quadRenderer = &quadRenderer;
  m_assetStore = &assetStore;

  if (TTF_Init() != 0)
  {
    LOG_ERROR(std::string("Failed to initialize SDL_ttf: ") + TTF_GetError());
    m_initialized = false;
    return false;
  }

  auto font = assetStore.addFont("default", "assets/fonts/m6x11.ttf", 24);

  if (!font)
    return false;

  TTF_SetFontHinting(font, TTF_HINTING_MONO);

  m_initialized = true;
  return true;
}

void TextRenderer::shutdown()
{
  for (auto& [key, cached] : m_textCache)
  {
    if (cached.texture != 0)
      glDeleteTextures(1, &cached.texture);
  }

  m_textCache.clear();

  if (m_initialized)
    TTF_Quit();

  m_initialized = false;
  m_quadRenderer = nullptr;
  m_assetStore = nullptr;
}

bool TextRenderer::isInitialized() { return m_initialized; }

void TextRenderer::drawText(float x, float y, const std::string& text)
{
  drawText(x, y, text, "default", Colors::White);
}

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

  if (!m_quadRenderer || !m_assetStore)
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

    SDL_Surface* rgbaSurface =
        SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGBA32, 0);

    SDL_FreeSurface(surface);

    if (!rgbaSurface)
    {
      LOG_ERROR(std::string("Failed to convert text surface: ") +
                SDL_GetError());
      return;
    }

    GLuint texture = 0;

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA,
                 rgbaSurface->w,
                 rgbaSurface->h,
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 rgbaSurface->pixels);

    CachedText cached;
    cached.width = rgbaSurface->w;
    cached.height = rgbaSurface->h;
    cached.texture = texture;

    SDL_FreeSurface(rgbaSurface);

    auto [insertedIt, inserted] =
        m_textCache.emplace(cacheKey, std::move(cached));

    it = insertedIt;
  }

  TexturedQuad command;

  command.texture = it->second.texture;

  command.srcRect = SDL_Rect{
      0,
      0,
      it->second.width,
      it->second.height,
  };

  command.destRect = SDL_Rect{
      static_cast<int>(x),
      static_cast<int>(y),
      it->second.width,
      it->second.height,
  };

  command.textureWidth = it->second.width;
  command.textureHeight = it->second.height;

  command.tint = SDL_Color{
      255,
      255,
      255,
      255,
  };

  m_quadRenderer->drawImmediate(command);
}

} // namespace sfs
