#pragma once

#include "panels/panel.h"

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/rendering/3d/material.h"

#include "nlohmann/json.hpp"

#include <optional>
#include <string>
#include <utility>

// Edits the selected entity's components, drawn from reflection: any component
// the registry knows (the engine's, or a game module's) gets an editor built
// from its field descriptors and [[sfs::...]] attributes, with no hand-written
// UI. The entity's name (NameComponent) is shown as the panel's title field.
class InspectorPanel : public Panel
{
public:
  InspectorPanel() : Panel("Inspector") {}

protected:
  void drawContents(EditorContext& context) override;

private:
  // A selected material asset's editor.
  void drawMaterial(EditorContext& context, const sfs::AssetRecord& record);
  void drawScriptableObject(EditorContext& context, const sfs::AssetRecord& record);
  // For an entity in a prefab instance: which prefab, and Apply / Revert.
  void drawPrefabHeader(EditorContext& context, const sfs::Entity& entity);

  std::string m_componentFilter; // the Add Component search box
  sfs::AssetId m_materialId;     // the material being edited, as last read
  std::optional<sfs::MaterialAsset> m_material;
  sfs::AssetId m_scriptableId;                   // the scriptable object being edited, as last read
  std::optional<nlohmann::json> m_data;    // its file's contents

  // Prefab actions asked for while drawing, done once the panel is drawn.
  enum class InstanceAction { Apply, Revert };
  std::optional<InstanceAction> m_pendingInstanceAction;
  std::optional<std::pair<std::string, std::string>> m_pendingRevert; // component, field
};
