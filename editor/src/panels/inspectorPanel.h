#pragma once

#include "panels/panel.h"
#include "scene/unusedOverrides.h"

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/rendering/material.h"
#include "engine/core/rendering/terrain.h"
#include "engine/core/rendering/visualStyle.h"
#include "engine/runtime/assetStore/modelData.h"

#include "nlohmann/json.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sfs::reflect
{
struct FieldInfo;
} // namespace sfs::reflect

/**
 * An entity reference picker's search, kept between frames: the entities it
 * lists are looked up again only when the search text, the slot or the scene
 * (its history revision) changes.
 */
struct EntityPickerSearch
{
  /** The search box's text. */
  std::string filter;
  /** The (lower-case) text the lists below were made for. */
  std::string madeFor;
  /** The history revision they were made at. */
  uint64_t revision = ~0ull;
  /** The entity and field whose slot they were made for. */
  sfs::Entity holder;
  const sfs::reflect::FieldInfo* field = nullptr;
  /** The entities named as the field's part. */
  std::vector<sfs::Entity> named;
  /** The instance's (or prefab's) entities the search finds. */
  std::vector<sfs::Entity> own;
  /** The scene's entities the search finds. */
  std::vector<sfs::Entity> scene;
  /** There were more than it lists. */
  bool more = false;
};

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
  // A terrain material's layers and splat map (in drawMaterial()): true if
  // they changed.
  bool drawTerrain(EditorContext& context, const sfs::AssetRecord& record, sfs::TerrainAsset& terrain);
  void drawScriptableObject(EditorContext& context, const sfs::AssetRecord& record);
  // A selected visual style's editor (every edit saved and shown live).
  void drawVisualStyle(EditorContext& context, const sfs::AssetRecord& record);
  // For an entity in a prefab instance: which prefab, and Apply / Revert.
  void drawPrefabHeader(EditorContext& context, const sfs::Entity& entity);
  // For a model's node: its stable id and name path, dimmed, and a warning
  // when siblings share its name (its id then follows their order).
  void drawModelNode(EditorContext& context, const sfs::Entity& entity);
  // For an instance root: the saved changes naming entities its prefab no
  // longer has, each to reattach to an entity it has now, or to remove.
  void drawUnusedOverrides(EditorContext& context, const sfs::Entity& root);

  // A model's nodes and their name paths, as its file had them when it was
  // last written (`written`), last looked at `checked` (ImGui's clock).
  struct ModelNodes
  {
    sfs::AssetId model;
    std::filesystem::file_time_type written;
    double checked = 0.0;
    std::vector<sfs::ModelNode> nodes;
    std::vector<std::string> paths;
  };
  // The model a node of an instance came from, and its nodes (read from its
  // file once, again when it changes); null if `entity` isn't a model's node
  // or the file can't be read.
  const ModelNodes* modelNodesOf(EditorContext& context,
                                 const sfs::Entity& entity);

  std::string m_componentFilter; // the Add Component search box
  std::string m_tagFilter;       // the Add Tag search box
  EntityPickerSearch m_pickSearch;        // an entity reference picker's search
  std::string m_renamedFrom;              // Reattach to...: the node's old name
  std::optional<ModelNodes> m_modelNodes; // the last model's, as read
  // The selected instance's unused overrides, as last read (at `revision` of
  // the history), with each one's line of text, and whether the instance has
  // model nodes (so a reattach can name a node's old name).
  struct UnusedList
  {
    sfs::Entity root;
    uint64_t revision = ~0ull;
    std::vector<unusedOverrides::Unused> items;
    std::vector<std::string> labels;
    bool modelNodes = false;
  };
  UnusedList m_unused;
  sfs::AssetId m_materialId;     // the material being edited, as last read
  std::optional<sfs::MaterialAsset> m_material;
  sfs::AssetId m_scriptableId;                   // the scriptable object being edited, as last read
  std::optional<nlohmann::json> m_data;    // its file's contents
  sfs::AssetId m_styleId;                  // the visual style being edited, as last read
  std::optional<sfs::VisualStyle> m_style;
  std::string m_newOverrideName; // the style's Material Overrides' Add row
  std::string m_newVariantTag;             // the material Variants' Add row

  // Prefab actions asked for while drawing, done once the panel is drawn.
  enum class InstanceAction { Apply, Revert };
  std::optional<InstanceAction> m_pendingInstanceAction;
  std::optional<std::pair<std::string, std::string>> m_pendingRevert; // component, field
  std::map<std::string, std::string>
      m_pendingReattach; // unused paths: old -> new
  std::optional<unusedOverrides::Unused> m_pendingRemoveUnused;
};
