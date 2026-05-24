#pragma once

#include "engine/Color/Color.h"
#include "engine/assetStore/assetStore.h"
#include "engine/rendering/openGLQuadRenderer.h"
#include <SDL_render.h>

namespace sfs
{

struct CachedText
{
  int width = 0;
  int height = 0;
  GLuint texture = 0;
};

class TextRenderer
{
public:
  static bool init(OpenGLQuadRenderer& quadRenderer, AssetStore& assetStore);
  static void shutdown();

  static bool isInitialized();

  static void drawText(float x, float y, const std::string& text);

  static void drawText(float x, float y, const std::string& text, Color color);

  static void drawText(float x,
                       float y,
                       const std::string& text,
                       const std::string& fontId,
                       Color color = Colors::White);

private:
  static bool m_initialized;
  static AssetStore* m_assetStore;
  static OpenGLQuadRenderer* m_quadRenderer;

  static std::unordered_map<std::string, CachedText> m_textCache;
  static std::string buildCacheKey(const std::string& text,
                                   const std::string& fontId,
                                   Color color);
};

} // namespace sfs
