#include "panels/materialEditorPanel.h"

#include "assets/assetDrop.h"
#include "assets/materialFiles.h"
#include "editorContext.h"
#include "scene/editorScene.h"
#include "widgets/widgets.h"

#include "engine/core/logger/logger.h"
#include "engine/core/rendering/3d/materialNodes.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/rendering3d/meshLibrary.h"
#include "engine/runtime/rendering3d/renderSystem3D.h"
#include "engine/webgpu/webGpuContext.h"

#include "imgui.h"
#include "imgui_internal.h" // NavWindow, RootWindowDockTree, FindWindowByName

#include <algorithm>
#include <chrono>

namespace mg = sfs::materialGraph;

namespace
{

constexpr float kPreviewSize = 192.0f; // points across, at most

const char* kAlphaModes[] = {"Opaque", "Cut-out (mask)", "See-through (blend)"};

} // namespace

MaterialEditorPanel::MaterialEditorPanel(sfs::WebGpuContext& gpu,
                                         const sfs::AssetRecord& record)
    : Panel(record.path.stem().string() + "###material-" +
            record.id.toString()),
      m_material(record.id), m_name(record.path.generic_string()),
      m_preview(gpu), m_opened(std::chrono::steady_clock::now())
{
}

bool MaterialEditorPanel::load(EditorContext& context)
{
  const sfs::AssetRecord* record =
      context.assets ? context.assets->find(m_material) : nullptr;
  const std::optional<sfs::MaterialAsset> asset =
      record ? materialFiles::read(*context.assets, *record) : std::nullopt;
  if (!asset || !asset->graph)
  {
    LOG_ERROR("Can't open " + m_name + " as a node-graph material");
    return false;
  }
  m_asset = m_saved = m_committed = *asset;
  m_undo.clear();
  m_redo.clear();
  m_applied = {}; // compile it next frame
  m_canvas.frame();
  return true;
}

bool MaterialEditorPanel::save(EditorContext& context)
{
  const sfs::AssetRecord* record =
      context.assets ? context.assets->find(m_material) : nullptr;
  if (!record || !materialFiles::write(*context.assets, *record, m_asset))
  {
    LOG_ERROR("Can't save " + m_name);
    return false;
  }
  m_saved = m_asset;
  context.assetFilesChanged = true; // its preview in the Assets panel
  if (context.scene)
    context.scene->assetStore().forgetMaterial(m_material);
  if (context.render)
    context.render->meshes().reloadMaterial("asset:" + m_material.toString());
  LOG_INFO("Saved the material " + m_name);
  return true;
}

void MaterialEditorPanel::revertScene(EditorContext& context) const
{
  if (context.scene)
    context.scene->assetStore().forgetMaterial(m_material);
  if (context.render)
    context.render->meshes().reloadMaterial("asset:" + m_material.toString());
}

void MaterialEditorPanel::undo()
{
  if (m_undo.empty())
    return;
  m_redo.push_back(m_asset);
  m_asset = m_committed = m_undo.back();
  m_undo.pop_back();
}

void MaterialEditorPanel::redo()
{
  if (m_redo.empty())
    return;
  m_undo.push_back(m_asset);
  m_asset = m_committed = m_redo.back();
  m_redo.pop_back();
}

void MaterialEditorPanel::recordUndo()
{
  // Mid-gesture (dragging nodes, a link, a slider), the edit isn't finished.
  if (m_canvas.busy() || ImGui::IsAnyItemActive() || m_asset == m_committed)
    return;
  m_undo.push_back(m_committed);
  m_redo.clear();
  m_committed = m_asset;
}

void MaterialEditorPanel::applyChanges(EditorContext& context)
{
  // A different scene (Stop, another scene opened, a prefab's tab) loaded the
  // material from its file: show it the unsaved edits too.
  const bool sceneChanged = context.scene != m_appliedTo;
  if (m_asset == m_applied && !sceneChanged)
    return;
  m_appliedTo = context.scene;
  const bool edited = m_asset != m_applied;
  if (edited)
  {
    m_applied = m_asset;
    m_compiled = mg::compile(*m_asset.graph);
  }
  if (!context.scene)
    return;
  // The preview and the scene show the edit at once; a graph with errors
  // keeps showing what it last compiled to, rather than magenta.
  std::shared_ptr<sfs::MeshMaterial> material;
  if (m_compiled.shader)
    material = context.scene->assetStore().makeMaterial(m_asset, m_name);
  if (material && edited)
    m_preview.setMaterial(*material);
  if (!context.render)
    return;
  // Back as saved (undone), the scene shows the file again.
  if (!unsaved())
  {
    if (edited)
      revertScene(context);
  }
  else if (material)
    context.render->meshes().setMaterial(
        "asset:" + m_material.toString(), *material);
}

void MaterialEditorPanel::update(EditorContext& context)
{
  // Its tab's X: close, unless there's something to save first.
  if (!open() && !m_finished)
  {
    if (unsaved())
    {
      open() = true;
      m_askClose = true;
    }
    else
      m_finished = true;
  }
}

void MaterialEditorPanel::beforeBegin()
{
  // Asking whether to save: bring the tab forward, where the question shows.
  if (m_askClose)
    ImGui::SetNextWindowFocus();
  if (!m_dockWith.empty())
  {
    if (const ImGuiWindow* other = ImGui::FindWindowByName(m_dockWith.c_str());
        other && other->DockId)
      ImGui::SetNextWindowDockID(other->DockId, ImGuiCond_Always);
    m_dockWith.clear();
  }
  if (!m_dockBelow.empty())
  {
    // Split the window's dock space as dragging a tab to its bottom edge
    // would: what's there moves up, this tab goes below.
    if (const ImGuiWindow* other = ImGui::FindWindowByName(m_dockBelow.c_str());
        other && other->DockId)
    {
      ImGuiID below = 0;
      ImGui::DockBuilderSplitNode(
          other->DockId, ImGuiDir_Down, 0.5f, &below, nullptr);
      ImGui::SetNextWindowDockID(below, ImGuiCond_Always);
    }
    m_dockBelow.clear();
  }
}

int MaterialEditorPanel::windowFlags() const
{
  return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
         (unsaved() ? ImGuiWindowFlags_UnsavedDocument : 0);
}

void MaterialEditorPanel::drawContents(EditorContext& context)
{
  if (!m_asset.graph)
    return;

  // Whose the Edit menu is: this panel's while it, or one of its parts, was
  // the last panel focused (menus and popups don't count).
  if (const ImGuiWindow* focused = ImGui::GetCurrentContext()->NavWindow)
    if (!(focused->Flags &
          (ImGuiWindowFlags_Popup | ImGuiWindowFlags_ChildMenu |
           ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_Tooltip)))
      m_ownsEdit = focused->RootWindowDockTree ==
                   ImGui::GetCurrentWindow()->RootWindowDockTree;

  handleKeys(context);
  drawToolbar(context);
  ImGui::Separator();

  // The canvas | a splitter | the sidebar. (Not a ResizeX child: that keeps
  // whatever width its first frame had, before the tab is docked.)
  const ImVec2 available = ImGui::GetContentRegionAvail();
  constexpr float kSplitter = 6.0f;
  m_sidebarWidth =
      std::clamp(m_sidebarWidth, 200.0f, std::max(200.0f, available.x * 0.6f));
  ImGui::BeginChild(
      "Graph",
      ImVec2(std::max(available.x - m_sidebarWidth - kSplitter, 50.0f),
             available.y),
      ImGuiChildFlags_None,
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
          ImGuiWindowFlags_NoMove);
  m_canvas.draw(*m_asset.graph, m_compiled, context.assets);
  ImGui::EndChild();
  ImGui::SameLine(0.0f, 0.0f);
  ImGui::InvisibleButton("##splitter", ImVec2(kSplitter, available.y));
  if (ImGui::IsItemActive())
    m_sidebarWidth -= ImGui::GetIO().MouseDelta.x;
  if (ImGui::IsItemHovered() || ImGui::IsItemActive())
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  ImGui::SameLine(0.0f, 0.0f);
  ImGui::BeginChild("Sidebar", ImVec2(0.0f, available.y));
  drawSidebar(context);
  ImGui::EndChild();

  applyChanges(context);
  recordUndo();
  drawCloseDialog(context);
}

void MaterialEditorPanel::handleKeys(EditorContext& context)
{
  // Ctrl+S works even while typing in a field.
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S))
    save(context);
  if (ImGui::GetIO().WantTextInput)
    return;
  mg::Graph& graph = *m_asset.graph;
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z))
    undo();
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y) ||
      ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z))
    redo();
  if (ImGui::Shortcut(ImGuiKey_Delete) || ImGui::Shortcut(ImGuiKey_Backspace))
    m_canvas.deleteSelection(graph);
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C))
    m_canvas.copySelection(graph);
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V))
    m_canvas.paste(graph);
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D))
    m_canvas.duplicateSelection(graph);
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A))
    m_canvas.selectAll(graph);
  if (ImGui::Shortcut(ImGuiKey_F))
    m_canvas.frame();
  if (ImGui::Shortcut(ImGuiKey_Space))
    m_canvas.openAddMenu();
}

void MaterialEditorPanel::drawToolbar(EditorContext& context)
{
  ImGui::BeginDisabled(!unsaved());
  if (ImGui::Button("Save"))
    save(context);
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("Write the material's file (Ctrl+S)");
  ImGui::SameLine();
  if (ImGui::Button("Frame"))
    m_canvas.frame();
  ImGui::SetItemTooltip("Fit the selected nodes, or every node, in view (F)");
  ImGui::SameLine();
  int mode = static_cast<int>(m_asset.alphaMode);
  ImGui::SetNextItemWidth(ImGui::CalcTextSize(kAlphaModes[2]).x +
                          ImGui::GetFrameHeight() * 2.0f);
  if (ImGui::Combo("##transparency", &mode, kAlphaModes, 3))
    m_asset.alphaMode = static_cast<sfs::AlphaMode>(mode);
  ImGui::SetItemTooltip("Transparency: opaque; cut out where Alpha is below "
                        "Alpha Clip (leaves, fences); or\n"
                        "see-through by Alpha (glass, water)");
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  if (m_compiled.errors.empty())
    ImGui::TextDisabled(
        "Right-click or Space to add nodes; drag between pins to link them");
  else
    ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.35f, 1.0f),
                       "%zu problem%s: the scene shows the last working graph",
                       m_compiled.errors.size(),
                       m_compiled.errors.size() == 1 ? "" : "s");
}

void MaterialEditorPanel::drawPreview()
{
  // At most its usual size, and under half the panel's height, so the node's
  // values below stay in view when the panel is short.
  const ImVec2 room = ImGui::GetContentRegionAvail();
  const float width =
      std::max(std::min({room.x, kPreviewSize, room.y * 0.45f}), 48.0f);
  const float seconds =
      std::chrono::duration<float>(std::chrono::steady_clock::now() - m_opened)
          .count();
  m_preview.draw(width, seconds);

  // The shape it's on, in a row under it (beside it, a narrow sidebar would
  // cut them off).
  const char* shapes[] = {"Sphere", "Cube", "Plane"};
  const char* uses[] = {
      "Every angle at once: shine, reflections and rims (Fresnel)",
      "Flat faces meeting at edges: how it wraps around corners (Triplanar)",
      "One flat surface: tiling, patterns and bumps"};
  for (int i = 0; i < 3; ++i)
  {
    if (i > 0)
      ImGui::SameLine();
    if (ImGui::RadioButton(shapes[i], static_cast<int>(m_preview.shape()) == i))
      m_preview.setShape(static_cast<MaterialPreview::Shape>(i));
    ImGui::SetItemTooltip("%s", uses[i]);
  }
}

void MaterialEditorPanel::drawSidebar(EditorContext& context)
{
  drawPreview();

  // What's wrong, each with a way to find it.
  if (!m_compiled.errors.empty())
  {
    ImGui::SeparatorText("Problems");
    for (const mg::CompileError& error : m_compiled.errors)
    {
      ImGui::PushID(&error);
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.45f, 0.4f, 1.0f));
      ImGui::TextWrapped("%s", error.message.c_str());
      ImGui::PopStyleColor();
      if (error.node && ImGui::SmallButton("Show"))
      {
        m_canvas.select(error.node);
        m_canvas.frame();
      }
      ImGui::PopID();
    }
  }

  const std::set<mg::NodeId>& selection = m_canvas.selection();
  if (selection.size() != 1)
  {
    ImGui::SeparatorText("Node");
    if (selection.empty())
      ImGui::TextDisabled("Select a node to change its values");
    else
      ImGui::TextDisabled("%zu nodes selected", selection.size());
    return;
  }
  if (mg::Node* node = m_asset.graph->find(*selection.begin()))
    drawNodeValues(context, *node);
}

void MaterialEditorPanel::drawNodeValues(EditorContext& context, mg::Node& node)
{
  const mg::NodeType* type = mg::findNodeType(node.type);
  if (!type)
  {
    ImGui::TextWrapped(
        "\"%s\" isn't a node this version knows.", node.type.c_str());
    return;
  }
  ImGui::SeparatorText(type->label.c_str());
  ImGui::PushStyleColor(
      ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", type->description.c_str());
  ImGui::PopStyleColor();

  // Room on the right for the longest label.
  float labelWidth = ImGui::CalcTextSize("Image").x;
  for (const mg::InputPin& pin : type->inputs)
    labelWidth = std::max(labelWidth, ImGui::CalcTextSize(pin.name.c_str()).x);
  ImGui::PushItemWidth(
      -(labelWidth + ImGui::GetStyle().ItemInnerSpacing.x * 2.0f));
  switch (type->property)
  {
  case mg::Property::Color:
    widgets::colorEdit4("Colour", &node.value.x);
    break;
  case mg::Property::Number:
    ImGui::DragFloat("Value", &node.value.x, 0.01f);
    break;
  case mg::Property::Texture:
  {
    const sfs::MeshLibrary* meshes =
        context.render ? &context.render->meshes() : nullptr;
    const std::string preview = node.texture.empty() ? "None"
                                : meshes ? meshes->label(node.texture)
                                         : node.texture;
    if (ImGui::BeginCombo("Image", preview.c_str()))
    {
      if (ImGui::Selectable("None", node.texture.empty()))
        node.texture.clear();
      if (meshes)
        for (const auto& [option, label] : meshes->availableTextures())
        {
          ImGui::PushID(option.c_str());
          if (ImGui::Selectable(label.c_str(), option == node.texture))
            node.texture = option;
          ImGui::PopID();
        }
      ImGui::EndCombo();
    }
    // Or one dropped from the Assets panel.
    if (context.assets && ImGui::BeginDragDropTarget())
    {
      const sfs::AssetRecord* dragged =
          assetDrop::draggedAsset(ImGui::GetDragDropPayload(), *context.assets);
      if (dragged && dragged->kind == sfs::AssetKind::Texture &&
          ImGui::AcceptDragDropPayload(assetDrop::kAssetPayload))
        node.texture = "asset:" + dragged->id.toString();
      ImGui::EndDragDropTarget();
    }
    if (!type->dataTexture) // a normal map is always data
    {
      ImGui::Checkbox("Data, not colour", &node.dataTexture);
      ImGui::SetItemTooltip("Read the image's numbers as they are (masks, "
                            "roughness, metalness), not as\n"
                            "a colour picture (which is converted from sRGB)");
    }
    break;
  }
  case mg::Property::None:
    break;
  }

  // The inputs' own values, used while nothing is linked to them.
  const mg::ValueType dynamic = [&]
  {
    const auto it = m_compiled.dynamicTypes.find(node.id);
    return it != m_compiled.dynamicTypes.end() ? it->second
                                               : mg::ValueType::Float;
  }();
  for (uint32_t i = 0; i < type->inputs.size() && i < node.inputs.size(); ++i)
  {
    const mg::InputPin& pin = type->inputs[i];
    const char* label = pin.name.c_str();
    ImGui::PushID(static_cast<int>(i));
    if (const mg::Link* link = m_asset.graph->linkInto(node.id, i))
    {
      // Linked: where it comes from, and a way to cut it.
      const mg::Node* source = m_asset.graph->find(link->fromNode);
      const mg::NodeType* sourceType =
          source ? mg::findNodeType(source->type) : nullptr;
      if (ImGui::SmallButton("Unlink"))
        m_asset.graph->disconnect(node.id, i);
      ImGui::SameLine();
      ImGui::TextDisabled(
          "%s, from %s", label, sourceType ? sourceType->label.c_str() : "?");
    }
    else if (pin.fallback != mg::Fallback::Value)
      ImGui::TextDisabled("%s: %s", label, mg::fallbackName(pin.fallback));
    else
    {
      glm::vec4& value = node.inputs[i];
      const mg::ValueType inputType = type->inputType(i, dynamic);
      if (pin.color && inputType == mg::ValueType::Vec3)
        widgets::colorEdit3(label, &value.x);
      else if (pin.unit)
        ImGui::SliderFloat(label, &value.x, 0.0f, 1.0f);
      else
        switch (inputType)
        {
        case mg::ValueType::Float:
          ImGui::DragFloat(label, &value.x, 0.01f);
          break;
        case mg::ValueType::Vec2:
          ImGui::DragFloat2(label, &value.x, 0.01f);
          break;
        case mg::ValueType::Vec3:
          ImGui::DragFloat3(label, &value.x, 0.01f);
          break;
        case mg::ValueType::Vec4:
          ImGui::DragFloat4(label, &value.x, 0.01f);
          break;
        }
    }
    ImGui::PopID();
  }
  ImGui::PopItemWidth();
}

void MaterialEditorPanel::drawCloseDialog(EditorContext& context)
{
  constexpr const char* kDialog = "Save material?";
  if (std::exchange(m_askClose, false))
    ImGui::OpenPopup(kDialog);
  if (!ImGui::BeginPopupModal(
          kDialog, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;
  ImGui::Text("Save your changes to %s before closing it?", m_name.c_str());
  ImGui::Spacing();
  if (ImGui::Button("Save") && save(context))
  {
    m_finished = true;
    open() = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button("Discard"))
  {
    revertScene(context);
    m_finished = true;
    open() = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel"))
    ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}
