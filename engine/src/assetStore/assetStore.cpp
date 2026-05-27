#include "engine/assetStore/assetStore.h"

#include "engine/assetStore/sprite.h"
#include "engine/logger/logger.h"
#include "engine/utils/string.h"

#include <SDL_image.h>

#include <fstream>
#include <nlohmann/json.hpp>
#include <utility>

namespace sfs
{

using json = nlohmann::json;

AssetStore::~AssetStore() { clearAssets(); }

void AssetStore::clearAssets()
{
  m_surfaces.clear();
  m_sprites.clear();
  m_spriteNameToId.clear();
  m_fonts.clear();

  LOG_DEBUG("Asset store cleared.");
}

void AssetStore::addTexture(const std::string& assetId,
                            const std::string& filePath)
{
  SDL_Surface* loadedSurface = IMG_Load(filePath.c_str());

  if (!loadedSurface)
  {
    LOG_INFO(std::string("Failed to load surface: ") + filePath +
             " | SDL_image error: " + IMG_GetError());
    return;
  }

  SDL_Surface* surface =
      SDL_ConvertSurfaceFormat(loadedSurface, SDL_PIXELFORMAT_RGBA32, 0);

  SDL_FreeSurface(loadedSurface);

  if (!surface)
  {
    LOG_INFO(std::string("Failed to convert surface: ") + filePath +
             " | SDL error: " + SDL_GetError());
    return;
  }

  m_surfaces.insert_or_assign(assetId, SurfacePtr(surface, SDL_FreeSurface));

  LOG_DEBUG("Stored surface with ID: " + assetId);
}

void AssetStore::removeTexture(const std::string& assetId)
{
  m_surfaces.erase(assetId);
}

SDL_Surface* AssetStore::getSurface(const std::string& assetId) const
{
  auto it = m_surfaces.find(assetId);

  if (it == m_surfaces.end())
    return nullptr;

  return it->second.get();
}

SpriteId AssetStore::addSprite(const std::string& textureId,
                               const std::string& spriteName,
                               SDL_Rect srcRect)
{
  const auto id = m_nextSpriteId++;
  auto sn = toLower(spriteName);

  m_sprites.emplace(id, Sprite{id, textureId, sn, srcRect});
  m_spriteNameToId[sn] = id;

  LOG_DEBUG("Created sprite: " + sn);

  return id;
}

SpriteId AssetStore::getOrCreateSprite(const std::string& spriteName,
                                       const std::string& path,
                                       SDL_Rect src)
{
  auto sprite = getSprite(spriteName);

  SpriteId spriteId;

  if (!sprite)
  {
    addTexture(spriteName, path);
    spriteId = addSprite(spriteName, spriteName, src);
  }
  else
  {
    spriteId = sprite->id;
  }
  return spriteId;
}

std::pair<SpriteId, SpriteId>
AssetStore::getOrCreateSpriteWithNormal(const std::string& spriteName,
                                        const std::string& path,
                                        SDL_Rect src,
                                        const std::string& normal)
{
  std::string normalPath = normal;
  if (normal == "")
  {
    size_t dot = path.find_last_of('.');
    normalPath = path.substr(0, dot) + "_normal" + path.substr(dot);
  }

  return std::make_pair(
      getOrCreateSprite(spriteName, path, src),
      getOrCreateSprite(spriteName + "_normal", normalPath, src));
}

std::vector<SpriteId>
AssetStore::addSprites(const std::string& textureId,
                       const std::vector<SpriteRegion>& regions)
{
  std::vector<SpriteId> ids;

  for (const auto& region : regions)
  {
    ids.push_back(addSprite(textureId, region.name, region.srcRect));
  }

  return ids;
}

SpriteId AssetStore::addSpriteFromSheet(const std::string& textureId,
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

std::vector<SpriteId>
AssetStore::addSpritesFromSheet(const std::string& textureId,
                                const std::string& baseSpriteName,
                                uint16_t width,
                                uint16_t height,
                                uint8_t gap,
                                uint8_t padding)
{
  SDL_Surface* surface = getSurface(textureId);

  if (!surface)
  {
    LOG_ERROR("Surface not found for sprite sheet: " + textureId);
    return {};
  }

  const int textureWidth = surface->w;
  const int textureHeight = surface->h;

  const int cols = (textureWidth - padding * 2 + gap) / (width + gap);
  const int rows = (textureHeight - padding * 2 + gap) / (height + gap);

  std::vector<SpriteId> ids;

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

  if (!file)
  {
    LOG_ERROR("Failed to open atlas JSON: " + jsonPath);
    return;
  }

  json data;
  file >> data;

  for (auto& [key, value] : data["frames"].items())
  {
    const auto& frame = value["frame"];

    SDL_Rect srcRect{frame["x"], frame["y"], frame["w"], frame["h"]};

    std::string name = key;

    auto start = name.find("(");
    auto end = name.find(")");

    if (start == std::string::npos || end == std::string::npos || end <= start)
    {
      addSprite(textureId, name, srcRect);
      continue;
    }

    std::string plant = name.substr(start + 1, end - start - 1);

    int stage = 0;

    try
    {
      stage = std::stoi(name.substr(end + 2));
    }
    catch (...)
    {
      addSprite(textureId, plant, srcRect);
      continue;
    }

    std::string cleanName = plant + "_stage_" + std::to_string(stage);

    addSprite(textureId, cleanName, srcRect);
  }
}

void AssetStore::removeSprite(SpriteId spriteId)
{
  auto it = m_sprites.find(spriteId);

  if (it == m_sprites.end())
    return;

  m_spriteNameToId.erase(it->second.name);
  m_sprites.erase(it);
}

const Sprite* AssetStore::getSprite(SpriteId spriteId) const
{
  auto it = m_sprites.find(spriteId);

  if (it == m_sprites.end())
    return nullptr;

  return &it->second;
}

const Sprite* AssetStore::getSprite(const std::string& spriteId) const
{
  auto it = m_spriteNameToId.find(toLower(spriteId));

  if (it == m_spriteNameToId.end())
    return nullptr;

  return getSprite(it->second);
}

TTF_Font* AssetStore::addFont(const std::string& fontId,
                              const std::string& path,
                              int size)
{
  TTF_Font* rawFont = TTF_OpenFont(path.c_str(), size);

  if (!rawFont)
  {
    LOG_ERROR("Failed to load font at " + path + " : " + TTF_GetError());
    return nullptr;
  }

  m_fonts.insert_or_assign(fontId, FontPtr(rawFont, TTF_CloseFont));

  return rawFont;
}

void AssetStore::removeFont(const std::string& fontId)
{
  m_fonts.erase(fontId);
}

TTF_Font* AssetStore::getFont(const std::string& fontId) const
{
  auto it = m_fonts.find(fontId);

  if (it == m_fonts.end())
  {
    LOG_ERROR("Font not found: " + fontId);
    return nullptr;
  }

  return it->second.get();
}

void AssetStore::addWhitePixelTexture(const std::string& textureId)
{
  SDL_Surface* surface =
      SDL_CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA32);

  if (!surface)
  {
    LOG_ERROR("Failed to create white pixel surface");
    return;
  }

  Uint32 white = SDL_MapRGBA(surface->format, 255, 255, 255, 255);
  SDL_FillRect(surface, nullptr, white);

  m_surfaces.insert_or_assign(
      textureId,
      std::unique_ptr<SDL_Surface, void (*)(SDL_Surface*)>(
          surface, SDL_FreeSurface));
}

} // namespace sfs
