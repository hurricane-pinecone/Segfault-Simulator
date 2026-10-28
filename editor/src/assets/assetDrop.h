#pragma once

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/ecs/entity.h"

#include <cstddef>
#include <string>

class EditorHistory;
struct ImGuiPayload;

// Dragging assets out of the Assets panel onto things: the payload, and
// dropping textures and materials into an object's material slots.
namespace assetDrop
{

// An Assets-panel drag: the dragged file's absolute path, NUL-terminated.
inline constexpr const char* kAssetPayload = "SFS_ASSET_PATH";

/** The asset a drag carries, or null (not an asset drag, or not in `assets`). */
const sfs::AssetRecord* draggedAsset(const ImGuiPayload* payload, const sfs::AssetDatabase& assets);

/** Whether the asset can go in a material slot: a material, or a texture. */
bool fitsMaterialSlot(const sfs::AssetRecord& record);

/**
 * The material name ("asset:<id>") a dropped asset puts in a slot: a material
 * itself, or for a texture a plain material with that texture (reusing one in
 * materials/ that is just that, else making one). Empty if it's neither.
 */
std::string materialFor(sfs::AssetDatabase& assets, const sfs::AssetRecord& record);

/** Put a material in one slot of an entity's Mesh Renderer, as one undoable edit. */
void setSlot(const sfs::Entity& entity, std::size_t slot, const std::string& material, EditorHistory* history);

} // namespace assetDrop
