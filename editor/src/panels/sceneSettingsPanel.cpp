#include "panels/sceneSettingsPanel.h"

#include "editorContext.h"
#include "history/editorHistory.h"
#include "scene/editorScene.h"
#include "widgets/widgets.h"

#include "engine/runtime/rendering/debug/ui.h"
#include "engine/runtime/rendering3d/renderSystem3D.h"
#include "engine/runtime/scripting/scriptRegistry.h"

#include <algorithm>

#include "imgui.h"

namespace
{
// The scene's game systems: System scripts it runs when it plays.
void gameSystems(EditorScene& scene, EditorHistory& history)
{
  std::vector<std::string>& systems = scene.gameSystems();

  if (!ImGui::CollapsingHeader("Game Systems", ImGuiTreeNodeFlags_DefaultOpen))
    return;

  for (std::size_t i = 0; i < systems.size(); ++i)
  {
    ImGui::PushID(static_cast<int>(i));
    const bool known = sfs::scripts().find(systems[i]) != nullptr;
    if (ImGui::SmallButton("x"))
    {
      history.change([&] { systems.erase(systems.begin() + static_cast<std::ptrdiff_t>(i)); });
      ImGui::PopID();
      break;
    }
    ImGui::SameLine();
    if (known)
      ImGui::TextUnformatted(systems[i].c_str());
    else
      ImGui::TextDisabled("%s (not built)", systems[i].c_str());
    ImGui::PopID();
  }
  if (systems.empty())
    ImGui::TextDisabled("None");

  if (ImGui::BeginCombo("##addSystem", "Add System"))
  {
    bool any = false;
    for (const sfs::ScriptType& type : sfs::scripts().types())
    {
      if (type.kind != sfs::ScriptType::Kind::System ||
          std::find(systems.begin(), systems.end(), type.name) != systems.end())
        continue;
      any = true;
      if (ImGui::Selectable(type.name.c_str()))
        history.change([&] { systems.push_back(type.name); });
    }
    if (!any)
      ImGui::TextDisabled("No other systems (create one under Scripts, then Build)");
    ImGui::EndCombo();
  }
}
} // namespace

void SceneSettingsPanel::drawContents(EditorContext& context)
{
  if (!context.render || !context.scene)
    return;

  EditorHistory& history = *context.history;
  gameSystems(*context.scene, history);
  ImGui::Separator();

  sfs::RenderSystem3D& render = *context.render;

  // Sun direction and colours aren't ModuleSettings (they're vectors), so they
  // get their own widgets; the rest comes from the descriptors.
  sfs::SceneLighting3D lighting = render.lighting();
  bool lightingChanged = false;
  lightingChanged |= widgets::undoable(&history, lighting.sun.direction, [](glm::vec3& v)
                                       { return ImGui::DragFloat3("Sun Direction", &v.x, 0.01f); });
  lightingChanged |= widgets::undoable(&history, lighting.sun.color, [](glm::vec3& v)
                                       { return ImGui::ColorEdit3("Sun Color", &v.x); });
  lightingChanged |= widgets::undoable(&history, lighting.skyColor, [](glm::vec3& v)
                                       { return ImGui::ColorEdit3("Sky Color", &v.x); });
  if (lightingChanged)
    render.setLighting(lighting);

  // The descriptor widgets apply their values themselves, so they can't edit a
  // copy. Instead, on a click frame each one speculatively snapshots before it
  // draws; the widget that takes the click keeps that snapshot until it's
  // released (one undo step per drag), and the others drop theirs unchanged.
  bool clickTaken = !ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
  for (const sfs::ModuleSetting& setting : render.settings())
  {
    const bool speculate = !clickTaken && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if (speculate)
      history.beginChange();

    sfs::renderModuleSetting(setting);

    if (ImGui::IsItemActive())
      clickTaken = true; // this widget owns the edit until it's released
    else if (speculate || ImGui::IsItemDeactivated())
      history.endChange(); // records nothing if the value didn't change
  }

  ImGui::Separator();
  const sfs::RenderStats3D& stats = render.stats();
  ImGui::Text("Draw calls: %zu", stats.drawCalls);
  ImGui::Text("Objects: %zu (%zu casting shadows)", stats.instances, stats.shadowInstances);
  ImGui::Text("Point lights: %zu", stats.pointLights);
}
