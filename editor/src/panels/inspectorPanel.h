#pragma once

#include "panels/panel.h"

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/rendering/3d/material.h"

#include <optional>
#include <string>

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

  std::string m_componentFilter; // the Add Component search box
  sfs::AssetId m_materialId;     // the material being edited, as last read
  std::optional<sfs::MaterialAsset> m_material;
};
