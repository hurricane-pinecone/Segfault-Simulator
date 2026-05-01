#include "assetStore.h"
#include "assetStore/sprite.h"
#include "logger/logger.h"
#include <SDL_image.h>
#include <SDL_render.h>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

void AssetStore::clearAssets()
{
  textures.clear();

  LOG_DEBUG("Asset store textures cleared.");
}

void AssetStore::addTexture(const std::string& assetId,
                            const std::string& filePath)
{
  SDL_Texture* texture = IMG_LoadTexture(&renderer, filePath.c_str());
  if (!texture)
  {
    LOG_ERROR(std::string("Failed to create texture ") + IMG_GetError());
    return;
  }

  textures.emplace(assetId, TexturePtr(texture, SDL_DestroyTexture));
  LOG_DEBUG("Created (sprite) with ID: " + assetId);
}

void AssetStore::removeTexture(const std::string& assetId)
{
  auto it = textures.find(assetId);

  if (it == textures.end())
    return;

  textures.erase(it);
}

SDL_Texture* AssetStore::getTexture(const std::string& assetId) const
{
  auto it = textures.find(assetId);
  if (it == textures.end())
    return nullptr;
  return it->second.get();
};

uint32_t AssetStore::addSpriteFromSpriteSheet(const std::string& sheetId,
                                              const std::string& spriteName,
                                              uint16_t width,
                                              uint16_t height,
                                              SpriteSheetPosition position)
{
  auto id = nextSpriteId++;
  sprites.emplace(std::pair<uint32_t, Sprite>(
      id, Sprite{id, sheetId, spriteName, width, height, position}));

  return id;
}

std::vector<uint32_t>
AssetStore::addSpritesFromSpriteSheet(const std::string& sheetId,
                                      const std::string& baseSpriteName,
                                      uint16_t width,
                                      uint16_t height,
                                      uint8_t gap)
{
  auto texture = getTexture(sheetId);
  int textureWidth;
  int textureHeight;
  SDL_QueryTexture(texture, nullptr, nullptr, &textureWidth, &textureHeight);

  int cols = textureWidth / (width + gap);
  int rows = textureHeight / (height + gap);

  std::vector<uint32_t> ids;

  for (int y = 0; y < rows; y++)
  {
    for (int x = 0; x < cols; x++)
    {
      int count = y * cols + x;
      std::string name = baseSpriteName + "_" + std::to_string(count);

      uint32_t id = addSpriteFromSpriteSheet(
          sheetId, name, width, height, SpriteSheetPosition{x, y, gap});

      LOG_DEBUG("Loaded sprite with ID: " + std::to_string(id));

      ids.push_back(id);
    }
  }
  return ids;
}

void AssetStore::removeSprite(uint32_t spriteId)
{
  auto it = sprites.find(spriteId);
  if (it == sprites.end())
    return;
  sprites.erase(it);
}

const Sprite* AssetStore::getSprite(uint32_t spriteId) const
{
  auto it = sprites.find(spriteId);

  if (it == sprites.end())
    return nullptr;

  return &it->second;
}
