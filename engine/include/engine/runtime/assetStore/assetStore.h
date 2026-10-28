#pragma once

#include "engine/core/assets/assetDatabase.h"
#include "engine/runtime/assetStore/modelData.h"
#include "engine/runtime/assetStore/sprite.h"

#include <SDL_rect.h>
#include <SDL_surface.h>
#include <SDL_ttf.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sfs
{

using SurfacePtr = std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)>;
using FontPtr = std::unique_ptr<TTF_Font, decltype(&TTF_CloseFont)>;

/**
 * The loaded assets a game draws with: images (as CPU surfaces), sprites
 * (named regions of an image), fonts and models. Renderers keep their own GPU
 * copies, keyed the same way.
 *
 * Images, sprites and fonts are added from file paths under names the game
 * picks. Given the project's AssetDatabase, the store also loads assets by
 * their AssetId (models, materials, and textures for 3D), keeping each until
 * cleared.
 */
class AssetStore
{
public:
  AssetStore() = default;
  ~AssetStore();

  void clearAssets();

  /**
   * The catalogue to load assets by id from (the project's assets/); null for
   * none. Changing it forgets the models loaded from the previous one.
   */
  void setDatabase(const AssetDatabase* database);
  const AssetDatabase* database() const { return m_database; }

  /**
   * A model asset's meshes, imported (with its .meta import settings) on first
   * use and kept. Null (logged once) if the id isn't a model in the database
   * or its file can't be imported.
   */
  const ModelData* model(const AssetId& id);

  /** The model if it's already loaded, without loading it. */
  const ModelData* loadedModel(const AssetId& id) const;

  /** Forget a loaded model, so the next model() reads its file again. */
  void forgetModel(const AssetId& id);

  /**
   * A texture asset's texels (for 3D), read (with its .meta import settings)
   * on first use and kept. Null (logged once) if the id isn't a texture in the
   * database or its file can't be read.
   */
  std::shared_ptr<const TextureData> texture(const AssetId& id);

  /**
   * A material asset (.mat), with its textures loaded, read on first use and
   * kept. Null (logged once) if the id isn't a material in the database or
   * its file can't be read.
   */
  std::shared_ptr<const MeshMaterial> material(const AssetId& id);

  /** Forget a loaded material, so the next material() reads its file again. */
  void forgetMaterial(const AssetId& id);

  void addTexture(const std::string& assetId, const std::string& filePath);
  void removeTexture(const std::string& assetId);
  SDL_Surface* getSurface(const std::string& assetId) const;

  SpriteId addSprite(const std::string& textureId,
                     const std::string& spriteName,
                     SDL_Rect srcRect);
  SpriteId getOrCreateSprite(const std::string& spriteName,
                             const std::string& path,
                             SDL_Rect src);
  std::pair<SpriteId, SpriteId>
  getOrCreateSpriteWithNormal(const std::string& spriteName,
                              const std::string& path,
                              SDL_Rect src,
                              const std::string& normal = "");

  std::vector<SpriteId> addSprites(const std::string& textureId,
                                   const std::vector<SpriteRegion>& regions);

  SpriteId addSpriteFromSheet(const std::string& textureId,
                              const std::string& spriteName,
                              uint16_t width,
                              uint16_t height,
                              uint16_t col,
                              uint16_t row,
                              uint8_t gap,
                              uint8_t padding);

  std::vector<SpriteId> addSpritesFromSheet(const std::string& textureId,
                                            const std::string& baseSpriteName,
                                            uint16_t width,
                                            uint16_t height,
                                            uint8_t gap,
                                            uint8_t padding);

  void loadAsepriteAtlas(const std::string& textureId,
                         const std::string& jsonPath);

  void removeSprite(SpriteId spriteId);
  const Sprite* getSprite(SpriteId spriteId) const;
  const Sprite* getSprite(const std::string& spriteId) const;

  TTF_Font*
  addFont(const std::string& fontId, const std::string& filePath, int size);

  void removeFont(const std::string& fontId);
  TTF_Font* getFont(const std::string& fontId) const;

  AssetStore(const AssetStore&) = delete;
  AssetStore& operator=(const AssetStore&) = delete;

  void addWhitePixelTexture(const std::string& textureId);

  // Generates a soft round texture: white RGB with a radial alpha falloff (1 at
  // the centre, 0 at the edge). Handy for particles/decals so they don't read
  // as hard squares. `size` is the texture's pixel dimension.
  void addRadialTexture(const std::string& textureId, int size);

private:
  std::unordered_map<std::string, SurfacePtr> m_surfaces;
  std::unordered_map<std::string, FontPtr> m_fonts;

  const AssetDatabase* m_database = nullptr;
  // nullopt: tried and failed (so it isn't retried, or logged, every time).
  std::unordered_map<AssetId, std::optional<ModelData>, AssetIdHash> m_models;
  // Null: tried and failed.
  std::unordered_map<AssetId, std::shared_ptr<const TextureData>, AssetIdHash> m_textures;
  std::unordered_map<AssetId, std::shared_ptr<const MeshMaterial>, AssetIdHash> m_materials;

  SpriteId m_nextSpriteId = 0;

  std::unordered_map<SpriteId, Sprite> m_sprites;
  std::unordered_map<std::string, SpriteId> m_spriteNameToId;
};

} // namespace sfs
