#pragma once

#include "engine/assetStore/sprite.h"

#include <SDL_rect.h>
#include <SDL_surface.h>
#include <SDL_ttf.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace sfs
{

using SurfacePtr = std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)>;
using FontPtr = std::unique_ptr<TTF_Font, decltype(&TTF_CloseFont)>;

class AssetStore
{
public:
  AssetStore() = default;
  ~AssetStore();

  void clearAssets();

  void addTexture(const std::string& assetId, const std::string& filePath);
  void removeTexture(const std::string& assetId);
  SDL_Surface* getSurface(const std::string& assetId) const;

  uint32_t addSprite(const std::string& textureId,
                     const std::string& spriteName,
                     SDL_Rect srcRect);

  std::vector<uint32_t> addSprites(const std::string& textureId,
                                   const std::vector<SpriteRegion>& regions);

  uint32_t addSpriteFromSheet(const std::string& textureId,
                              const std::string& spriteName,
                              uint16_t width,
                              uint16_t height,
                              uint16_t col,
                              uint16_t row,
                              uint8_t gap,
                              uint8_t padding);

  std::vector<uint32_t> addSpritesFromSheet(const std::string& textureId,
                                            const std::string& baseSpriteName,
                                            uint16_t width,
                                            uint16_t height,
                                            uint8_t gap,
                                            uint8_t padding);

  void loadAsepriteAtlas(const std::string& textureId,
                         const std::string& jsonPath);

  void removeSprite(uint32_t spriteId);
  const Sprite* getSprite(uint32_t spriteId) const;
  const Sprite* getSprite(const std::string& spriteId) const;

  TTF_Font*
  addFont(const std::string& fontId, const std::string& filePath, int size);

  void removeFont(const std::string& fontId);
  TTF_Font* getFont(const std::string& fontId) const;

  AssetStore(const AssetStore&) = delete;
  AssetStore& operator=(const AssetStore&) = delete;

private:
  std::unordered_map<std::string, SurfacePtr> m_surfaces;
  std::unordered_map<std::string, FontPtr> m_fonts;

  uint32_t m_nextSpriteId = 0;

  std::unordered_map<uint32_t, Sprite> m_sprites;
  std::unordered_map<std::string, uint32_t> m_spriteNameToId;
};

} // namespace sfs
