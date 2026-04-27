#pragma once

#include <SDL_render.h>
#include <memory>
#include <string>
#include <unordered_map>

class AssetStore
{
public:
  AssetStore(SDL_Renderer& renderer) : renderer(renderer) {};
  ~AssetStore() { clearAssets(); };

  void clearAssets();
  void addTexture(const std::string& assetId, const std::string& filePath);
  void removeTexture(const std::string& assetId);
  SDL_Texture* getTexture(const std::string& assetId) const;

  AssetStore(const AssetStore&) = delete;
  AssetStore& operator=(const AssetStore&) = delete;

private:
  using TexturePtr =
      std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)>;

  SDL_Renderer& renderer;

  std::unordered_map<std::string, TexturePtr> textures;
};
