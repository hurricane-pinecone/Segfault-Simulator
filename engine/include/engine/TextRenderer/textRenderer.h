#pragma once

#include "SDL2/SDL_render.h"
#include "engine/Color/Color.h"
#include "engine/assetStore/assetStore.h"
#include "engine/types/SDLPtrs.h"
#include <SDL_render.h>
namespace sfs
{

struct CachedText
{
  int width = 0;
  int height = 0;
  TexturePtr texture{nullptr, SDL_DestroyTexture};
};

class TextRenderer
{
public:
  static bool init(SDL_Renderer& renderer, AssetStore& assetStore);
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
  static SDL_Renderer* m_renderer;
  static AssetStore* m_assetStore;

  static std::unordered_map<std::string, CachedText> m_textCache;
  static std::string buildCacheKey(const std::string& text,
                                   const std::string& fontId,
                                   Color color);
};

} // namespace sfs
