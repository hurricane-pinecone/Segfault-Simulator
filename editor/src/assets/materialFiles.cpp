#include "assets/materialFiles.h"

#include "engine/core/logger/logger.h"
#include "engine/runtime/assetStore/textureImporter.h"
#include "engine/core/util/atomicFile.h"

#include <cctype>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace materialFiles
{

namespace
{
// `stem` + `extension` in `folder` (relative), with " 2", " 3"... added until
// no file has that name.
fs::path freeName(const sfs::AssetDatabase& assets,
                  const fs::path& folder,
                  const std::string& stem,
                  const char* extension)
{
  fs::path relative = folder / (stem + extension);
  std::error_code error;
  for (int n = 2; fs::exists(assets.root() / relative, error); ++n)
    relative = folder / (stem + " " + std::to_string(n) + extension);
  return relative;
}

// A file name made of letters, digits, spaces, '-' and '_'.
std::string safeName(std::string name)
{
  for (char& c : name)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != ' ' && c != '-' &&
        c != '_')
      c = '_';
  return name.empty() ? "Material" : name;
}

// Save a texture beside the material as a texture asset; its name, or empty.
std::string saveTexture(sfs::AssetDatabase& assets,
                        const fs::path& folder,
                        const std::shared_ptr<const sfs::TextureData>& texture,
                        const std::string& stem)
{
  if (!texture)
    return {};
  const fs::path relative = freeName(assets, folder, stem, ".png");
  if (!sfs::saveTexturePng(*texture, assets.root() / relative))
    return {};
  assets.refresh();
  const sfs::AssetRecord* record = assets.findByPath(relative);
  if (!record)
    return {};
  if (texture->nearest)
    assets.setSettings(record->id, {{"filter", "nearest"}});
  return "asset:" + record->id.toString();
}
} // namespace

std::optional<sfs::AssetId> create(sfs::AssetDatabase& assets,
                                   const fs::path& folder,
                                   const std::string& name,
                                   const sfs::MaterialAsset& material)
{
  std::error_code error;
  fs::create_directories(assets.root() / folder, error);
  const fs::path relative = freeName(assets, folder, safeName(name), ".mat");
  if (!sfs::writeFileAtomically(assets.root() / relative, material.toJson().dump(2) + "\n"))
  {
    LOG_ERROR("Can't write the material " + relative.generic_string());
    return std::nullopt;
  }
  assets.refresh();
  if (const sfs::AssetRecord* record = assets.findByPath(relative))
    return record->id;
  return std::nullopt;
}

std::optional<sfs::MaterialAsset> read(const sfs::AssetDatabase& assets,
                                       const sfs::AssetRecord& record)
{
  std::ifstream in(assets.absolutePath(record));
  const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
  std::optional<sfs::MaterialAsset> material =
      sfs::MaterialAsset::fromJson(json);
  if (!material)
    LOG_ERROR("Can't read the material " + record.path.generic_string());
  return material;
}

bool write(const sfs::AssetDatabase& assets,
           const sfs::AssetRecord& record,
           const sfs::MaterialAsset& material)
{
  if (sfs::writeFileAtomically(assets.absolutePath(record), material.toJson().dump(2) + "\n"))
    return true;
  LOG_ERROR("Can't save the material " + record.path.generic_string());
  return false;
}

std::optional<sfs::AssetId> extract(sfs::AssetDatabase& assets,
                                    const sfs::MeshMaterial& material,
                                    const std::string& name)
{
  const fs::path folder = "materials";
  std::error_code error;
  fs::create_directories(assets.root() / folder, error);

  sfs::MaterialAsset asset;
  asset.baseColor = material.baseColor;
  asset.tiling = material.tiling;
  asset.offset = material.offset;
  asset.metallic = material.metallic;
  asset.roughness = material.roughness;
  asset.alphaMode = material.alphaMode;
  asset.alphaCutoff = material.alphaCutoff;
  const std::string stem = safeName(name);
  asset.baseTexture =
      saveTexture(assets, folder, material.baseTexture, stem + "_base");
  asset.metallicRoughnessTexture = saveTexture(
      assets, folder, material.metallicRoughnessTexture, stem + "_metalRough");
  asset.normalTexture =
      saveTexture(assets, folder, material.normalTexture, stem + "_normal");
  asset.normalScale = material.normalScale;
  return create(assets, folder, stem, asset);
}

} // namespace materialFiles
