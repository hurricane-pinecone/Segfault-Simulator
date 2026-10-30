#include "panels/sceneSettingsPanel.h"

#include "editorContext.h"
#include "history/editorHistory.h"
#include "scene/editorScene.h"
#include "widgets/widgets.h"

#include "engine/core/components/nameComponent.h"
#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/runtime/rendering/debug/ui.h"
#include "engine/runtime/rendering3d/renderSystem3D.h"
#include "engine/runtime/scripting/scriptRegistry.h"

#include <algorithm>

#include "imgui.h"

namespace
{
// The systems the scene runs when it plays: the engine's optional ones (such as
// physics) and the game's System scripts.
void gameSystems(EditorScene& scene, EditorHistory& history)
{
  std::vector<std::string>& systems = scene.gameSystems();

  if (!ImGui::CollapsingHeader("Systems", ImGuiTreeNodeFlags_DefaultOpen))
    return;

  for (std::size_t i = 0; i < systems.size(); ++i)
  {
    ImGui::PushID(static_cast<int>(i));
    const bool known = sfs::scripts().find(systems[i]) != nullptr;
    if (ImGui::SmallButton("x"))
    {
      history.change(
          [&]
          { systems.erase(systems.begin() + static_cast<std::ptrdiff_t>(i)); });
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
    // The engine's first, then the game's.
    std::vector<const sfs::ScriptType*> candidates;
    for (const sfs::ScriptType& type : sfs::scripts().types())
      if (type.kind == sfs::ScriptType::Kind::System &&
          std::find(systems.begin(), systems.end(), type.name) == systems.end())
        candidates.push_back(&type);
    std::stable_sort(candidates.begin(),
                     candidates.end(),
                     [](const sfs::ScriptType* a, const sfs::ScriptType* b) {
                       return (a->module == "engine") > (b->module == "engine");
                     });

    const std::string* section = nullptr;
    for (const sfs::ScriptType* type : candidates)
    {
      if (!section || *section != type->module)
      {
        section = &type->module;
        ImGui::SeparatorText(*section == "engine" ? "Engine"
                                                  : section->c_str());
      }
      if (ImGui::Selectable(type->name.c_str()))
        history.change([&] { systems.push_back(type->name); });
    }
    if (candidates.empty())
      ImGui::TextDisabled(
          "No other systems (create one under Scripts, then Build)");
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

  // Which view the game starts from.
  if (const sfs::Entity camera = render.sceneCamera(); camera.isValid())
  {
    const bool named = camera.hasComponent<sfs::NameComponent>();
    ImGui::Text("Game camera: %s",
                named ? camera.getComponent<sfs::NameComponent>().name.c_str()
                      : "(unnamed)");
  }
  else
  {
    ImGui::TextDisabled("Game camera: none");
    ImGui::SetItemTooltip(
        "Without a camera entity, the game starts from the viewport's\n"
        "view as last saved. Add one with + > Camera.");
  }
  ImGui::Separator();

  // Sun direction and colours aren't ModuleSettings (they're vectors), so they
  // get their own widgets; the rest comes from the descriptors.
  sfs::SceneLighting3D lighting = render.lighting();
  bool lightingChanged = false;
  lightingChanged |= widgets::undoable(
      &history,
      lighting.sun.direction,
      [](glm::vec3& v)
      { return ImGui::DragFloat3("Sun Direction", &v.x, 0.01f); });
  lightingChanged |= widgets::undoable(
      &history,
      lighting.sun.color,
      [](glm::vec3& v) { return widgets::colorEdit3("Sun Color", &v.x); });
  lightingChanged |= widgets::undoable(
      &history,
      lighting.skyColor,
      [](glm::vec3& v) { return widgets::colorEdit3("Sky Color", &v.x); });
  if (lightingChanged)
    render.setLighting(lighting);

  // The descriptor widgets apply their values themselves, so they can't edit a
  // copy. Instead, on a click frame each one speculatively snapshots before it
  // draws; the widget that takes the click keeps that snapshot until it's
  // released (one undo step per drag), and the others drop theirs unchanged.
  bool clickTaken = !ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
  for (const sfs::ModuleSetting& setting : render.settings())
  {
    const bool speculate =
        !clickTaken && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
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
  const float framerate = ImGui::GetIO().Framerate;
  ImGui::Text("Frame: %.1f ms (%.0f fps)",
              framerate > 0.0f ? 1000.0f / framerate : 0.0f,
              framerate);
  ImGui::Text("Draw calls: %zu", stats.drawCalls);
  ImGui::Text("Objects: %zu (%zu casting shadows)",
              stats.instances,
              stats.shadowInstances);
  ImGui::Text("Point lights: %zu (%zu from emission, %zu casting shadows)",
              stats.pointLights,
              stats.emissionLights,
              stats.shadowedLights);
  ImGui::Text("Grass fields: %zu", stats.grassFields);
}
