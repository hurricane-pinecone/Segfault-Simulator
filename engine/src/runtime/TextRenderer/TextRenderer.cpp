#include "engine/core/Color/Color.h"
#include "engine/core/logger/logger.h"
#include "engine/core/rendering/quads.h"
#include "engine/runtime/rendering/iQuadRenderer.h"
#include "engine/runtime/rendering/util/sdlColor.h"
#include <SDL_hints.h>
#include <SDL_ttf.h>
#include <engine/runtime/TextRenderer/textRenderer.h>
#ifdef __EMSCRIPTEN__
  #include <GLES3/gl3.h>
#else
  #include <GL/glew.h>
#endif

namespace sfs
{

std::string TextRenderer::buildCacheKey(const std::string& text,
                                        const std::string& fontId,
                                        Color color)
{
  return fontId + "_" + std::to_string(color.r) + "_" +
         std::to_string(color.g) + "_" + std::to_string(color.b) + "_" +
         std::to_string(color.a) + "_" + text;
}

TextRenderer::TextRenderer(IQuadRenderer& quadRenderer, AssetStore& assetStore)
    : m_quadRenderer(quadRenderer), m_assetStore(assetStore)
{
}

TextRenderer::~TextRenderer()
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
}

bool TextRenderer::init()
{
  if (TTF_Init() != 0)
  {
    LOG_ERROR(std::string("Failed to initialize SDL_ttf: ") + TTF_GetError());
    m_initialized = false;
    return false;
  }

  auto font = m_assetStore.addFont("default", "assets/fonts/m6x11.ttf", 24);

  if (!font)
    return false;

  TTF_SetFontHinting(font, TTF_HINTING_MONO);

  // A smaller font for the dev console: a system monospace where one is
  // available, falling back to the bundled pixel font elsewhere (e.g. web).
  TTF_Font* consoleFont = nullptr;
#if defined(__APPLE__)
  consoleFont =
      m_assetStore.addFont("console", "/System/Library/Fonts/Menlo.ttc", 14);
#endif
  if (consoleFont)
  {
    TTF_SetFontHinting(consoleFont, TTF_HINTING_NORMAL);
  }
  else
  {
    consoleFont = m_assetStore.addFont("console", "assets/fonts/m6x11.ttf", 16);
    if (consoleFont)
      TTF_SetFontHinting(consoleFont, TTF_HINTING_MONO);
  }

  m_initialized = true;
  return true;
}

int TextRenderer::lineHeight(const std::string& fontId) const
{
  TTF_Font* font = m_assetStore.getFont(fontId);
  return font ? TTF_FontLineSkip(font) : 0;
}

bool TextRenderer::isInitialized() const { return m_initialized; }

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

  if (text.empty())
    return;

  TTF_Font* font = m_assetStore.getFont(fontId);

  if (!font)
    return;

  std::string cacheKey = buildCacheKey(text, fontId, color);

  auto it = m_textCache.find(cacheKey);

  if (it == m_textCache.end())
  {
    SDL_Surface* surface =
        TTF_RenderText_Blended(font, text.c_str(), toSDL(color));

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

  command.srcRect = {
      0,
      0,
      it->second.width,
      it->second.height,
  };

  command.destRect = {
      static_cast<int>(x),
      static_cast<int>(y),
      it->second.width,
      it->second.height,
  };

  command.textureWidth = it->second.width;
  command.textureHeight = it->second.height;

  command.tint = {
      255,
      255,
      255,
      255,
  };

  m_quadRenderer.drawImmediate(command);
}

} // namespace sfs
