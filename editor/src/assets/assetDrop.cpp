#include "assets/assetDrop.h"

#include "assets/materialFiles.h"
#include "history/editorHistory.h"

#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)

#include "imgui.h"

#include <filesystem>

namespace assetDrop
{

const sfs::AssetRecord* draggedAsset(const ImGuiPayload* payload, const sfs::AssetDatabase& assets)
{
  if (!payload || !payload->IsDataType(kAssetPayload) || !payload->Data)
    return nullptr;
  const std::filesystem::path file(static_cast<const char*>(payload->Data));
  return assets.findByPath(file.lexically_relative(assets.root()));
}

bool placeable(const sfs::AssetRecord& record)
{
  return record.kind == sfs::AssetKind::Prefab || record.kind == sfs::AssetKind::Model;
}

bool fitsMaterialSlot(const sfs::AssetRecord& record)
{
  return record.kind == sfs::AssetKind::Material || record.kind == sfs::AssetKind::Texture;
}

std::string materialFor(sfs::AssetDatabase& assets, const sfs::AssetRecord& record)
{
  if (record.kind == sfs::AssetKind::Material)
    return "asset:" + record.id.toString();
  if (record.kind != sfs::AssetKind::Texture)
    return {};

  sfs::MaterialAsset wanted;
  wanted.baseTexture = "asset:" + record.id.toString();
  for (const sfs::AssetRecord& other : assets.records())
    if (other.kind == sfs::AssetKind::Material && other.path.parent_path() == "materials")
      if (const auto existing = materialFiles::read(assets, other); existing && *existing == wanted)
        return "asset:" + other.id.toString();

  const auto id = materialFiles::create(assets, "materials", record.path.stem().string(), wanted);
  return id ? "asset:" + id->toString() : std::string();
}

void setSlot(const sfs::Entity& entity, std::size_t slot, const std::string& material, EditorHistory* history)
{
  if (!entity.hasComponent<sfs::MeshRendererComponent>())
    return;
  std::vector<std::string>& list = entity.getComponent<sfs::MeshRendererComponent>().materials;
  const auto apply = [&]
  {
    if (list.size() <= slot)
      list.resize(slot + 1);
    list[slot] = material;
    while (!list.empty() && list.back().empty())
      list.pop_back(); // trailing "own" slots aren't saved
  };
  if (history)
    history->change(apply);
  else
    apply();
}

} // namespace assetDrop
