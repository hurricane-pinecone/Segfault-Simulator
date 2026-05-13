#pragma once

#include "engine/types/SDLPtrs.h"
#include "sprite.h"
#include <SDL2/SDL_render.h>
#include <SDL_rect.h>
#include <SDL_ttf.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sfs
{

class AssetStore
{
public:
  explicit AssetStore(SDL_Renderer& renderer)
      : renderer(renderer) {

        };
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
  const Sprite* getSprite(const std::string& spriteId) const;

  TTF_Font*
  addFont(const std::string& fontId, const std::string& filePath, int size);
  void removeFont(const std::string& fontId);
  TTF_Font* getFont(const std::string& fontId) const;

  SDL_Surface* getSurface(const std::string& assetId) const;

  AssetStore(const AssetStore&) = delete;
  AssetStore& operator=(const AssetStore&) = delete;

private:
  SDL_Renderer& renderer;

  std::unordered_map<std::string, TexturePtr> textures;
  std::unordered_map<std::string, SurfacePtr> surfaces;

  std::unordered_map<std::string, FontPtr> fonts;

  // This looks wierd, but its more performant than storing strings as the main
  // sprite key (sprites).
  // The second map, spriteNameId allows sprite name searches while avoiding
  // expensive lookups in game loop.
  uint32_t nextSpriteId = 0;
  std::unordered_map<uint32_t, Sprite> sprites;
  std::unordered_map<std::string, uint32_t> spriteNameToId;
};

} // namespace sfs
