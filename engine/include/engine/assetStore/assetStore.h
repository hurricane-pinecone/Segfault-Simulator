#pragma once

#include "sprite.h"
#include <SDL2/SDL_render.h>
#include <SDL_rect.h>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace sfs
{

class AssetStore
{
public:
  AssetStore(SDL_Renderer& renderer) : renderer(renderer) {};
  ~AssetStore() { clearAssets(); };

  void clearAssets();
  void addTexture(const std::string& assetId, const std::string& filePath);
  void removeTexture(const std::string& assetId);
  SDL_Texture* getTexture(const std::string& assetId) const;

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

  AssetStore(const AssetStore&) = delete;
  AssetStore& operator=(const AssetStore&) = delete;

private:
  using TexturePtr =
      std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)>;

  SDL_Renderer& renderer;

  std::unordered_map<std::string, TexturePtr> textures;

  uint32_t nextSpriteId = 0;
  std::unordered_map<uint32_t, Sprite> sprites;
};

} // namespace sfs
