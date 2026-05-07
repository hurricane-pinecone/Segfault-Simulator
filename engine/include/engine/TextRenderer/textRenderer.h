#pragma once

#include "SDL2/SDL_render.h"
#include "engine/assetStore/assetStore.h"
namespace sfs
{

class TextRenderer
{
public:
  static bool init(SDL_Renderer& renderer, AssetStore& assetStore);
  static void shutdown();

  static bool isInitialized();

  static void drawText(float x, float y, const std::string& text);

  static void drawText(float x,
                       float y,
                       const std::string& text,
                       const std::string& fontId);

  static void drawText(float x,
                       float y,
                       const std::string& text,
                       const std::string& fontId,
                       SDL_Color color);

private:
  static bool m_initialized;
  static SDL_Renderer* m_renderer;
  static AssetStore* m_assetStore;
};
} // namespace sfs
