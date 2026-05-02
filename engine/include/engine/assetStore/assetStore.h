#pragma once

#include "sprite.h"
#include <SDL2/SDL_render.h>
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

  uint32_t addSpriteFromSpriteSheet(const std::string& sheetId,
                                    const std::string& spriteName,
                                    uint16_t width,
                                    uint16_t height,
                                    SpriteSheetPosition position);
  std::vector<uint32_t>
  addSpritesFromSpriteSheet(const std::string& sheetId,
                            const std::string& baseSpriteName,
                            uint16_t width,
                            uint16_t height,
                            uint8_t gap);
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
