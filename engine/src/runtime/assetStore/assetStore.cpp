#include "engine/runtime/assetStore/assetStore.h"

#include "engine/core/components/transformComponent.h"
#include "engine/core/jobs/workerPool.h"
#include "engine/core/logger/logger.h"
#include "engine/core/rendering/material.h"
#include "engine/core/rendering/materialGraphCompiler.h"
#include "engine/core/rendering/visualStyle.h"
#include "engine/runtime/assetStore/modelImporter.h"
#include "engine/runtime/assetStore/textureImporter.h"
#include "engine/runtime/serialization/legacyComponentNames.h"
#include "glm/glm/exponential.hpp"

#include <SDL_image.h>

#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <utility>

namespace sfs
{

using json = nlohmann::json;

AssetStore::AssetStore() = default;

AssetStore::~AssetStore()
{
  m_workers.reset(); // running loads finish before what they write to goes
  clearAssets();
}

void AssetStore::clearAssets()
{
  m_surfaces.clear();
  m_imageTextures.clear();
  m_fonts.clear();
  m_models.clear();
  m_textures.clear();
  m_dataTextures.clear();
  m_materials.clear();
  m_prefabs.clear();
  m_loading.clear(); // loads under way are dropped when they finish
  m_lastUse.clear();

  LOG_DEBUG("Asset store cleared.");
}

void AssetStore::setDatabase(const AssetDatabase* database)
{
  if (database != m_database)
  {
    m_models.clear();
    m_textures.clear();
    m_dataTextures.clear();
    m_materials.clear();
    m_prefabs.clear();
    m_loading.clear();
  }
  m_database = database;
}

const ModelData* AssetStore::model(const AssetId& id)
{
  used(id);
  if (const auto it = m_models.find(id); it != m_models.end())
    return it->second ? &*it->second : nullptr;

  const AssetRecord* record = m_database ? m_database->find(id) : nullptr;
  std::optional<ModelData> loaded;
  if (!record)
    LOG_ERROR("No model asset with id " + id.toString());
  else if (record->kind != AssetKind::Model)
    LOG_ERROR(record->path.generic_string() + " isn't a model");
  else
    loaded = importModel(m_database->absolutePath(*record),
                         ModelImportSettings::fromJson(record->settings));

  const auto [it, inserted] = m_models.emplace(id, std::move(loaded));
  return it->second ? &*it->second : nullptr;
}

const ModelData* AssetStore::loadedModel(const AssetId& id) const
{
  const auto it = m_models.find(id);
  return it != m_models.end() && it->second ? &*it->second : nullptr;
}

void AssetStore::forgetModel(const AssetId& id)
{
  m_models.erase(id);
  m_loading.erase(id);
}

void AssetStore::setBackgroundLoading(bool background)
{
  m_background = background;
  if (background && !m_workers)
    m_workers = std::make_unique<WorkerPool>(2);
}

AssetStore::LoadState AssetStore::requestModel(const AssetId& id,
                                               const ModelData** model)
{
  *model = nullptr;
  used(id);
  if (const auto it = m_models.find(id); it != m_models.end())
  {
    *model = it->second ? &*it->second : nullptr;
    return *model ? LoadState::Loaded : LoadState::Failed;
  }
  if (m_loading.contains(id))
    return LoadState::Loading;
  const AssetRecord* record = m_database ? m_database->find(id) : nullptr;
  if (!m_background || !m_workers || !record ||
      record->kind != AssetKind::Model)
  {
    *model = this->model(id); // loads now (or logs why it can't, and remembers)
    return *model ? LoadState::Loaded : LoadState::Failed;
  }

  // The worker gets everything it needs now: it mustn't read the database,
  // which the main thread may be changing.
  const uint64_t ticket = m_nextTicket++;
  m_loading[id] = ticket;
  if (m_progress.done == m_progress.total)
    m_progress = {};
  ++m_progress.total;
  m_workers->submit(
      [this,
       id,
       ticket,
       file = m_database->absolutePath(*record),
       settings = ModelImportSettings::fromJson(record->settings)]
      {
        std::optional<ModelData> loaded = importModel(file, settings);
        const std::lock_guard lock(m_finishedMutex);
        m_finished.push_back({id, ticket, std::move(loaded)});
      });
  return LoadState::Loading;
}

void AssetStore::collectLoads()
{
  std::vector<FinishedLoad> finished;
  {
    const std::lock_guard lock(m_finishedMutex);
    finished.swap(m_finished);
  }
  if (finished.empty())
    return;
  for (FinishedLoad& load : finished)
  {
    m_progress.done = std::min(m_progress.done + 1, m_progress.total);
    const auto it = m_loading.find(load.id);
    if (it == m_loading.end() || it->second != load.ticket)
      continue; // forgotten meanwhile
    m_loading.erase(it);
    m_models.emplace(
        load.id, std::move(load.model)); // unless loaded at once meanwhile
  }
  ++m_loadGeneration;
}

std::shared_ptr<const TextureData> AssetStore::texture(const AssetId& id)
{
  used(id);
  if (const auto it = m_textures.find(id); it != m_textures.end())
    return it->second;

  const AssetRecord* record = m_database ? m_database->find(id) : nullptr;
  std::shared_ptr<const TextureData> loaded;
  if (!record)
    LOG_ERROR("No texture asset with id " + id.toString());
  else if (record->kind != AssetKind::Texture)
    LOG_ERROR(record->path.generic_string() + " isn't a texture");
  else
    loaded = importTexture(m_database->absolutePath(*record),
                           TextureImportSettings::fromJson(record->settings));
  return m_textures.emplace(id, std::move(loaded)).first->second;
}

std::shared_ptr<const TextureData> AssetStore::dataTexture(const AssetId& id)
{
  used(id);
  if (const auto it = m_dataTextures.find(id); it != m_dataTextures.end())
    return it->second;
  std::shared_ptr<const TextureData> data = texture(id);
  if (data && !data->linear)
  {
    auto linear = std::make_shared<TextureData>(*data);
    linear->linear = true;
    data = std::move(linear);
  }
  return m_dataTextures.emplace(id, std::move(data)).first->second;
}

std::shared_ptr<const MeshMaterial> AssetStore::material(const AssetId& id)
{
  used(id);
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
    if (const std::optional<MaterialAsset> asset =
            MaterialAsset::fromJson(json))
      loaded = makeMaterial(*asset, record->path.generic_string());
    else
      LOG_ERROR("Can't read the material " + record->path.generic_string());
  }
  return m_materials.emplace(id, std::move(loaded)).first->second;
}

std::shared_ptr<MeshMaterial>
AssetStore::makeMaterial(const MaterialAsset& asset, const std::string& source)
{
  // Textures by asset name ("asset:<id>"), read as colour or as data
  // (metalness, roughness, masks).
  const auto texture = [this](const std::string& name,
                              bool data =
                                  false) -> std::shared_ptr<const TextureData>
  {
    constexpr std::string_view prefix = "asset:";
    if (!name.starts_with(prefix))
      return nullptr;
    const std::optional<AssetId> textureId =
        AssetId::parse(std::string_view(name).substr(prefix.size()));
    if (!textureId)
      return nullptr;
    return data ? dataTexture(*textureId) : this->texture(*textureId);
  };

  auto material = std::make_shared<MeshMaterial>();
  material->alphaMode = asset.alphaMode;
  material->variants.assign(asset.variants.begin(), asset.variants.end());
  if (asset.terrain)
  {
    // Colour as colour; normals, roughness and the splat map as data.
    auto terrain = std::make_shared<TerrainMaterial>();
    terrain->splat = texture(asset.terrain->splatMap, true);
    terrain->blendSharpness = asset.terrain->blendSharpness;
    terrain->antiTiling = asset.terrain->antiTiling;
    terrain->variation = asset.terrain->variation;
    for (const TerrainLayerAsset& layer : asset.terrain->layers)
      terrain->layers.push_back({layer.name,
                                 texture(layer.baseTexture),
                                 texture(layer.normalTexture, true),
                                 texture(layer.roughnessTexture, true),
                                 layer.tint,
                                 layer.tileSize,
                                 layer.roughness,
                                 layer.normalScale,
                                 layer.grass});
    material->alphaMode = AlphaMode::Opaque;
    material->terrain = std::move(terrain);
    return material;
  }
  if (asset.graph)
  {
    const materialGraph::CompiledGraph compiled =
        materialGraph::compile(*asset.graph);
    if (compiled.shader)
    {
      material->shader = compiled.shader;
      material->shaderParameters = compiled.parameters;
      for (const materialGraph::TextureSlot& slot : compiled.textures)
        material->shaderTextures.push_back(texture(slot.texture, slot.data));
    }
    else
    {
      // Magenta, so it's noticed.
      for (const materialGraph::CompileError& error : compiled.errors)
        LOG_ERROR(source + ": " + error.message);
      material->baseColor = {1.0f, 0.0f, 1.0f, 1.0f};
    }
    return material;
  }
  material->baseColor = asset.baseColor;
  material->baseTexture = texture(asset.baseTexture);
  material->metallicRoughnessTexture =
      texture(asset.metallicRoughnessTexture, true);
  material->normalTexture = texture(asset.normalTexture, true);
  material->normalScale = asset.normalScale;
  material->tiling = asset.tiling;
  material->offset = asset.offset;
  material->metallic = asset.metallic;
  material->roughness = asset.roughness;
  material->alphaCutoff = asset.alphaCutoff;
  return material;
}

void AssetStore::forgetMaterial(const AssetId& id) { m_materials.erase(id); }

std::optional<AssetId> AssetStore::textureId(const TextureData* texture) const
{
  if (!texture)
    return std::nullopt;
  for (const auto* textures : {&m_textures, &m_dataTextures})
    for (const auto& [id, loaded] : *textures)
      if (loaded.get() == texture)
        return id;
  return std::nullopt;
}

void AssetStore::forgetTexture(const AssetId& id)
{
  m_textures.erase(id);
  m_dataTextures.erase(id);
}

std::optional<nlohmann::json> AssetStore::prefab(const AssetId& id)
{
  used(id);
  if (const auto it = m_prefabs.find(id); it != m_prefabs.end())
    return it->second;

  const AssetRecord* record = m_database ? m_database->find(id) : nullptr;
  std::optional<nlohmann::json> loaded;
  if (!record)
    LOG_ERROR("No prefab asset with id " + id.toString());
  else if (record->kind == AssetKind::Model)
  {
    // Only its node tree: read from the file's structure, unless the model
    // is loaded already. Its meshes load separately (in the background, in an
    // editor), so placing a model doesn't wait for its geometry.
    std::optional<std::vector<ModelNode>> nodes;
    if (const ModelData* data = loadedModel(id))
      nodes = data->nodes;
    else
      nodes = importModelNodes(m_database->absolutePath(*record),
                               ModelImportSettings::fromJson(record->settings));
    if (nodes)
      loaded = modelPrefab(*nodes, id, record->path.stem().string());
  }
  else if (record->kind != AssetKind::Prefab)
    LOG_ERROR(record->path.generic_string() + " isn't a prefab");
  else
  {
    std::ifstream in(m_database->absolutePath(*record));
    nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
    upgradeComponentNames(json);
    if (json.is_object() && json.contains("root"))
      loaded = std::move(json);
    else
      LOG_ERROR("Can't read the prefab " + record->path.generic_string());
  }
  return m_prefabs.emplace(id, std::move(loaded)).first->second;
}

void AssetStore::forgetPrefab(const AssetId& id) { m_prefabs.erase(id); }

std::optional<VisualStyle> AssetStore::visualStyle(const AssetId& id) const
{
  const AssetRecord* record = m_database ? m_database->find(id) : nullptr;
  if (!record)
  {
    LOG_ERROR("No visual style asset with id " + id.toString());
    return std::nullopt;
  }
  if (record->kind != AssetKind::VisualStyle)
  {
    LOG_ERROR(record->path.generic_string() + " isn't a visual style");
    return std::nullopt;
  }
  std::ifstream in(m_database->absolutePath(*record));
  const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
  std::optional<VisualStyle> style = VisualStyle::fromJson(json);
  if (!style)
    LOG_ERROR("Can't read the visual style " + record->path.generic_string());
  return style;
}

void AssetStore::beginUseTracking() { ++m_useEpoch; }

std::size_t AssetStore::collectUnused()
{
  const auto unused = [this](const AssetId& id)
  {
    const auto it = m_lastUse.find(id);
    return it == m_lastUse.end() || it->second < m_useEpoch;
  };
  std::size_t dropped = 0;
  const auto drop = [&](auto& assets)
  {
    dropped += std::erase_if(
        assets, [&](const auto& entry) { return unused(entry.first); });
  };
  drop(m_models);
  drop(m_materials);
  drop(m_prefabs);

  // A kept material's textures stay (else asking for one again would read a
  // second copy).
  std::unordered_set<const TextureData*> kept;
  for (const auto& [id, material] : m_materials)
    if (material)
    {
      kept.insert(material->baseTexture.get());
      kept.insert(material->metallicRoughnessTexture.get());
      kept.insert(material->normalTexture.get());
      for (const auto& texture : material->shaderTextures)
        kept.insert(texture.get());
      if (material->terrain)
      {
        kept.insert(material->terrain->splat.get());
        for (const TerrainLayer& layer : material->terrain->layers)
        {
          kept.insert(layer.baseTexture.get());
          kept.insert(layer.normalTexture.get());
          kept.insert(layer.roughnessTexture.get());
        }
      }
    }
  const auto dropTextures = [&](auto& textures)
  {
    dropped += std::erase_if(
        textures,
        [&](const auto& entry)
        { return unused(entry.first) && !kept.contains(entry.second.get()); });
  };
  dropTextures(m_textures);
  dropTextures(m_dataTextures);

  std::erase_if(
      m_lastUse, [&](const auto& entry) { return entry.second < m_useEpoch; });
  if (dropped > 0)
    LOG_INFO("Let go of " + std::to_string(dropped) +
             " assets the new scene doesn't use");
  return dropped;
}

namespace
{
nlohmann::json transformJson(const glm::mat4& matrix)
{
  const TransformComponent t = TransformComponent::fromMatrix(matrix);
  return {
      {"position", {t.position.x, t.position.y, t.position.z}},
      {"rotation", {t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z}},
      {"scale", {t.scale.x, t.scale.y, t.scale.z}}};
}
} // namespace

nlohmann::json AssetStore::modelPrefab(const std::vector<ModelNode>& nodes,
                                       const AssetId& id,
                                       const std::string& name)
{
  nlohmann::json root = {
      {"NameComponent", {{"name", name}}},
      {"TransformComponent", transformJson(glm::mat4(1.0f))}};
  if (nodes.empty())
  {
    root["MeshRendererComponent"] = {{"mesh", assetMeshName(id)}};
    return {{"formatVersion", 1}, {"root", std::move(root)}};
  }

  // Each node an entity with its part, its $id the node's place in the file's
  // order (stable while the file's node order is).
  std::vector<nlohmann::json> entries(nodes.size());
  std::vector<std::vector<std::size_t>> children(nodes.size());
  std::vector<std::size_t> tops;
  for (std::size_t i = 0; i < nodes.size(); ++i)
  {
    const ModelNode& node = nodes[i];
    entries[i] = {{"$id", i + 1},
                  {"NameComponent", {{"name", node.name}}},
                  {"TransformComponent", transformJson(node.local)}};
    if (!node.part.empty())
      entries[i]["MeshRendererComponent"] = {
          {"mesh", assetMeshName(id, node.part)}};
    (node.parent < 0 ? tops : children[node.parent]).push_back(i);
  }
  const std::function<nlohmann::json(std::size_t)> build = [&](std::size_t i)
  {
    nlohmann::json entry = entries[i];
    for (const std::size_t child : children[i])
      entry["children"].push_back(build(child));
    return entry;
  };
  for (const std::size_t top : tops)
    root["children"].push_back(build(top));
  return {{"formatVersion", 1}, {"root", std::move(root)}};
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
  m_imageTextures.erase(assetId);
}

std::shared_ptr<const TextureData>
AssetStore::imageTexture(const std::string& assetId)
{
  if (assetId.starts_with("asset:"))
  {
    const std::optional<AssetId> id = AssetId::parse(assetId.substr(6));
    return id ? texture(*id) : nullptr;
  }
  if (const auto it = m_imageTextures.find(assetId);
      it != m_imageTextures.end())
    return it->second;
  SDL_Surface* surface = getSurface(assetId);
  std::shared_ptr<TextureData> texture =
      surface ? textureFromSurface(*surface) : nullptr;
  if (texture)
    texture->nearest = true;
  return m_imageTextures.emplace(assetId, std::move(texture)).first->second;
}

std::optional<AssetStore::SpriteFrame>
AssetStore::spriteFrame(const std::string& image, int frame)
{
  SpriteFrame out;
  out.sheet = imageTexture(image);
  if (!out.sheet || out.sheet->empty())
    return std::nullopt;
  const glm::vec2 texels(out.sheet->width, out.sheet->height);
  out.pixels = texels;

  // A texture asset may be a grid of frames, and have its own scale.
  glm::ivec2 cell{0, 0};
  if (image.starts_with("asset:") && m_database)
    if (const std::optional<AssetId> id = AssetId::parse(image.substr(6)))
      if (const AssetRecord* record = m_database->find(*id))
      {
        const TextureImportSettings settings =
            TextureImportSettings::fromJson(record->settings);
        cell = settings.spriteCell;
        out.pixelsPerUnit = settings.pixelsPerUnit;
      }
  const glm::ivec2 grid(cell.x > 0 ? int(texels.x) / cell.x : 0,
                        cell.y > 0 ? int(texels.y) / cell.y : 0);
  if (grid.x <= 0 || grid.y <= 0)
    return out;

  const int count = grid.x * grid.y;
  const int index = ((frame % count) + count) % count;
  const glm::vec2 size(cell);
  out.pixels = size;
  out.uvRect = glm::vec4(float(index % grid.x) * size.x / texels.x,
                         float(index / grid.x) * size.y / texels.y,
                         size.x / texels.x,
                         size.y / texels.y);
  return out;
}

SDL_Surface* AssetStore::getSurface(const std::string& assetId) const
{
  auto it = m_surfaces.find(assetId);

  if (it == m_surfaces.end())
    return nullptr;

  return it->second.get();
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
