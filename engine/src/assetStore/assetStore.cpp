#include "engine/utils/string.h"
#include <SDL.h>
#include <SDL_image.h>
#include <SDL_render.h>
#include <engine/assetStore/assetStore.h>
#include <engine/assetStore/sprite.h>
#include <engine/logger/logger.h>
#include <fstream>
#include <nlohmann/json.hpp>

namespace sfs
{

using json = nlohmann::json;

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
  LOG_DEBUG("Created texture with ID: " + assetId);
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

uint32_t AssetStore::addSprite(const std::string& textureId,
                               const std::string& spriteName,
                               SDL_Rect srcRect)
{
  const auto id = nextSpriteId++;
  auto sn = toLower(spriteName);

  sprites.emplace(id, Sprite{id, textureId, sn, srcRect});
  spriteNameToId[sn] = id;

  LOG_DEBUG("Created sprite: " + sn);

  return id;
}

std::vector<uint32_t>
AssetStore::addSprites(const std::string& textureId,
                       const std::vector<SpriteRegion>& regions)
{
  std::vector<uint32_t> ids;

  for (const auto& region : regions)
  {
    ids.push_back(addSprite(textureId, region.name, region.srcRect));
  }

  return ids;
}

uint32_t AssetStore::addSpriteFromSheet(const std::string& textureId,
                                        const std::string& spriteName,
                                        uint16_t width,
                                        uint16_t height,
                                        uint16_t col,
                                        uint16_t row,
                                        uint8_t gap,
                                        uint8_t padding)
{
  SDL_Rect srcRect{padding + col * (width + gap),
                   padding + row * (height + gap),
                   width,
                   height};

  return addSprite(textureId, spriteName, srcRect);
}

std::vector<uint32_t>
AssetStore::addSpritesFromSheet(const std::string& textureId,
                                const std::string& baseSpriteName,
                                uint16_t width,
                                uint16_t height,
                                uint8_t gap,
                                uint8_t padding)
{
  auto texture = getTexture(textureId);

  int textureWidth = 0;
  int textureHeight = 0;
  SDL_QueryTexture(texture, nullptr, nullptr, &textureWidth, &textureHeight);

  int cols = (textureWidth - padding * 2 + gap) / (width + gap);
  int rows = (textureHeight - padding * 2 + gap) / (height + gap);

  std::vector<uint32_t> ids;

  for (int row = 0; row < rows; row++)
  {
    for (int col = 0; col < cols; col++)
    {
      int count = row * cols + col;
      std::string name = baseSpriteName + "_" + std::to_string(count);

      ids.push_back(addSpriteFromSheet(
          textureId, name, width, height, col, row, gap, padding));
    }
  }

  return ids;
}

void AssetStore::loadAsepriteAtlas(const std::string& textureId,
                                   const std::string& jsonPath)
{
  std::ifstream file(jsonPath);
  json data;
  file >> data;

  for (auto& [key, value] : data["frames"].items())
  {
    const auto& frame = value["frame"];

    SDL_Rect srcRect{frame["x"], frame["y"], frame["w"], frame["h"]};

    // ---- Parse name ----
    // "Sprite-0001 (Grapes) 2." -> "graps_stage_2"
    std::string name = key;

    auto start = name.find("(");
    auto end = name.find(")");

    std::string plant = name.substr(start + 1, end - start - 1);

    int stage = std::stoi(name.substr(end + 2));

    std::string cleanName = plant + "_stage_" + std::to_string(stage);

    addSprite(textureId, cleanName, srcRect);
  }
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

const Sprite* AssetStore::getSprite(const std::string& spriteId) const
{
  auto it = spriteNameToId.find(toLower(spriteId));

  if (it == spriteNameToId.end())
    return nullptr;

  return getSprite(it->second);
}

} // namespace sfs
