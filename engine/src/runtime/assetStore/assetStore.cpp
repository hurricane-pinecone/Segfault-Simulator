#include "engine/runtime/assetStore/assetStore.h"

#include "engine/core/logger/logger.h"
#include "engine/core/rendering/3d/material.h"
#include "engine/core/util/string.h"
#include "engine/runtime/assetStore/modelImporter.h"
#include "engine/runtime/assetStore/textureImporter.h"
#include "engine/runtime/assetStore/sprite.h"
#include "glm/glm/exponential.hpp"

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
  m_models.clear();
  m_textures.clear();
  m_materials.clear();

  LOG_DEBUG("Asset store cleared.");
}

void AssetStore::setDatabase(const AssetDatabase* database)
{
  if (database != m_database)
  {
    m_models.clear();
    m_textures.clear();
    m_materials.clear();
  }
  m_database = database;
}

const ModelData* AssetStore::model(const AssetId& id)
{
  if (const auto it = m_models.find(id); it != m_models.end())
    return it->second ? &*it->second : nullptr;

  const AssetRecord* record = m_database ? m_database->find(id) : nullptr;
  std::optional<ModelData> loaded;
  if (!record)
    LOG_ERROR("No model asset with id " + id.toString());
  else if (record->kind != AssetKind::Model)
    LOG_ERROR(record->path.generic_string() + " isn't a model");
  else
    loaded = importModel(m_database->absolutePath(*record), ModelImportSettings::fromJson(record->settings));

  const auto [it, inserted] = m_models.emplace(id, std::move(loaded));
  return it->second ? &*it->second : nullptr;
}

const ModelData* AssetStore::loadedModel(const AssetId& id) const
{
  const auto it = m_models.find(id);
  return it != m_models.end() && it->second ? &*it->second : nullptr;
}

void AssetStore::forgetModel(const AssetId& id) { m_models.erase(id); }

std::shared_ptr<const TextureData> AssetStore::texture(const AssetId& id)
{
  if (const auto it = m_textures.find(id); it != m_textures.end())
    return it->second;

  const AssetRecord* record = m_database ? m_database->find(id) : nullptr;
  std::shared_ptr<const TextureData> loaded;
  if (!record)
    LOG_ERROR("No texture asset with id " + id.toString());
  else if (record->kind != AssetKind::Texture)
    LOG_ERROR(record->path.generic_string() + " isn't a texture");
  else
    loaded = importTexture(m_database->absolutePath(*record), TextureImportSettings::fromJson(record->settings));
  return m_textures.emplace(id, std::move(loaded)).first->second;
}

std::shared_ptr<const MeshMaterial> AssetStore::material(const AssetId& id)
{
  if (const auto it = m_materials.find(id); it != m_materials.end())
    return it->second;

  const AssetRecord* record = m_database ? m_database->find(id) : nullptr;
  std::shared_ptr<MeshMaterial> loaded;
  if (!record)
    LOG_ERROR("No material asset with id " + id.toString());
  else if (record->kind != AssetKind::Material)
    LOG_ERROR(record->path.generic_string() + " isn't a material");
  else
  {
    std::ifstream in(m_database->absolutePath(*record));
    const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
    if (const std::optional<MaterialAsset> asset = MaterialAsset::fromJson(json))
    {
      // Textures by asset name: "asset:<id>".
      const auto texture = [this](const std::string& name) -> std::shared_ptr<const TextureData>
      {
        constexpr std::string_view prefix = "asset:";
        if (!name.starts_with(prefix))
          return nullptr;
        const std::optional<AssetId> textureId = AssetId::parse(std::string_view(name).substr(prefix.size()));
        return textureId ? this->texture(*textureId) : nullptr;
      };
      loaded = std::make_shared<MeshMaterial>();
      loaded->baseColor = asset->baseColor;
      loaded->baseTexture = texture(asset->baseTexture);
      loaded->metallicRoughnessTexture = texture(asset->metallicRoughnessTexture);
      if (loaded->metallicRoughnessTexture && !loaded->metallicRoughnessTexture->linear)
      {
        // Read as data, not colour.
        auto linear = std::make_shared<TextureData>(*loaded->metallicRoughnessTexture);
        linear->linear = true;
        loaded->metallicRoughnessTexture = std::move(linear);
      }
      loaded->tiling = asset->tiling;
      loaded->offset = asset->offset;
      loaded->metallic = asset->metallic;
      loaded->roughness = asset->roughness;
      loaded->alphaMode = asset->alphaMode;
      loaded->alphaCutoff = asset->alphaCutoff;
    }
    else
      LOG_ERROR("Can't read the material " + record->path.generic_string());
  }
  return m_materials.emplace(id, std::move(loaded)).first->second;
}

void AssetStore::forgetMaterial(const AssetId& id) { m_materials.erase(id); }

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

void AssetStore::addRadialTexture(const std::string& textureId, int size)
{
  if (size < 2)
    size = 2;

  SDL_Surface* surface =
      SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_RGBA32);

  if (!surface)
  {
    LOG_ERROR("Failed to create radial texture surface");
    return;
  }

  SDL_LockSurface(surface);

  auto* pixels = static_cast<Uint32*>(surface->pixels);
  const int pitch = surface->pitch / 4;
  const float center = (static_cast<float>(size) - 1.0f) * 0.5f;
  const float radius = center > 0.0f ? center : 1.0f;

  for (int y = 0; y < size; ++y)
  {
    for (int x = 0; x < size; ++x)
    {
      const float dx = (static_cast<float>(x) - center) / radius;
      const float dy = (static_cast<float>(y) - center) / radius;
      const float dist = glm::sqrt(dx * dx + dy * dy); // 0 centre -> 1 edge

      float a = 1.0f - dist;
      if (a < 0.0f)
        a = 0.0f;
      a *= a; // soften the falloff toward the rim

      const Uint8 alpha = static_cast<Uint8>(a * 255.0f + 0.5f);
      pixels[y * pitch + x] =
          SDL_MapRGBA(surface->format, 255, 255, 255, alpha);
    }
  }

  SDL_UnlockSurface(surface);

  m_surfaces.insert_or_assign(
      textureId,
      std::unique_ptr<SDL_Surface, void (*)(SDL_Surface*)>(
          surface, SDL_FreeSurface));
}

} // namespace sfs
