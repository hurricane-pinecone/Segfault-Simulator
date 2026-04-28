#include "assetStore.h"
#include "logger/logger.h"
#include <SDL_image.h>
#include <SDL_render.h>
#include <memory>
#include <string>

void AssetStore::clearAssets()
{
  textures.clear();

  LOG_DEBUG("Asset store textures cleared.");
}

// TODO: Prevent adding the same texture twice somehow.
// IE: some-image.png should not be able to be added more than once.
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
