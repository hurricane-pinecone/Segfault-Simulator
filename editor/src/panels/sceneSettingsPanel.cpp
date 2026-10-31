#include "panels/sceneSettingsPanel.h"

#include "editorContext.h"
#include "history/editorHistory.h"
#include "scene/editorScene.h"
#include "widgets/lookWidgets.h"
#include "widgets/widgets.h"

#include "engine/core/components/nameComponent.h"
#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/runtime/rendering/debug/ui.h"
#include "engine/runtime/rendering/renderSystem.h"
#include "engine/runtime/rendering/visualStyles.h"
#include "engine/runtime/scripting/scriptRegistry.h"

#include <algorithm>
#include <filesystem>

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

// The scene's style: 3D, or a 2D game's plane (flat or isometric) and how its
// pixels map to the world. Only the settings the chosen style uses show.
void sceneStyle(sfs::RenderSystem& render, EditorHistory& history)
{
  if (!ImGui::CollapsingHeader("Style", ImGuiTreeNodeFlags_DefaultOpen))
    return;

  using Projection = sfs::Space2D::Projection;
  const std::optional<sfs::Space2D>& current = render.space2D();
  const char* kinds[] = {"3D", "Flat 2D", "Isometric 2D"};
  int kind = !current                                     ? 0
             : current->projection == Projection::Flat ? 1
                                                          : 2;
  widgets::propertyLabel("Style");
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::Combo("##style", &kind, kinds, 3))
    history.change(
        [&]
        {
          if (kind == 0)
          {
            render.clearSpace2D();
            return;
          }
          sfs::Space2D space = render.space2D().value_or(sfs::Space2D{});
          space.projection =
              kind == 1 ? Projection::Flat : Projection::Isometric;
          render.setSpace2D(space);
        });
  ImGui::SetItemTooltip(
      "3D: a world in metres, seen through a camera you place.\n"
      "Flat 2D: sprites on a plane, seen straight on (platformers,\n"
      "top-down games). Isometric 2D: a tile grid seen at an angle.");

  if (!render.space2D())
    return;
  sfs::Space2D space = *render.space2D();
  bool changed = widgets::undoable(&history,
                                   space.pixelsPerUnit,
                                   [](float& value)
                                   {
                                     widgets::propertyLabel("Pixels Per Unit");
                                     ImGui::SetNextItemWidth(-FLT_MIN);
                                     return ImGui::DragFloat("##ppu",
                                                             &value,
                                                             0.1f,
                                                             0.01f,
                                                             1000.0f);
                                   });
  if (space.projection == Projection::Flat)
  {
    changed |= widgets::undoable(
        &history,
        space.yDown,
        [](bool& value)
        { return widgets::propertyCheckbox("Y Runs Down", &value); });
    changed |= widgets::undoable(
        &history,
        space.ySort,
        [](bool& value)
        { return widgets::propertyCheckbox("Sort By Y", &value); });
  }
  else
  {
    changed |= widgets::undoable(&history,
                                 space.tilePixels,
                                 [](glm::vec2& value)
                                 {
                                   widgets::propertyLabel("Tile Pixels");
                                   ImGui::SetNextItemWidth(-FLT_MIN);
                                   return ImGui::DragFloat2(
                                       "##tile", &value.x, 0.5f, 1.0f, 512.0f);
                                 });
    changed |= widgets::undoable(&history,
                                 space.heightPixels,
                                 [](float& value)
                                 {
                                   widgets::propertyLabel("Height Pixels");
                                   ImGui::SetNextItemWidth(-FLT_MIN);
                                   return ImGui::DragFloat(
                                       "##height", &value, 0.5f, 0.0f, 512.0f);
                                 });
  }
  if (changed)
    render.setSpace2D(space);
}
// The scene's look: a visual style (a .style asset) or, with none, its own
// colour grading. A style's look replaces the scene's own while it's chosen.
void sceneLook(EditorContext& context,
               sfs::RenderSystem& render,
               EditorHistory& history)
{
  if (!ImGui::CollapsingHeader("Look", ImGuiTreeNodeFlags_DefaultOpen))
    return;

  const std::vector<std::pair<std::string, std::string>> styles =
      context.assets ? sfs::visualStyleAssets(*context.assets)
                     : std::vector<std::pair<std::string, std::string>>{};
  const std::string& current = render.visualStyleName();
  // A style shows by its file's name.
  const auto label = [](const std::string& path)
  { return std::filesystem::path(path).stem().string(); };
  std::string preview = "None (the scene's own)";
  for (const auto& [name, path] : styles)
    if (name == current)
      preview = label(path);
  if (!current.empty() && preview.rfind("None", 0) == 0)
    preview = "Missing (" + current + ")";

  widgets::propertyLabel("Visual Style");
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::BeginCombo("##visualStyle", preview.c_str()))
  {
    const auto choose = [&](const std::string& name)
    { history.change([&] { render.setVisualStyle(name); }); };
    if (ImGui::Selectable("None (the scene's own)", current.empty()))
      choose("");
    for (const auto& [name, path] : styles)
    {
      ImGui::PushID(name.c_str());
      if (ImGui::Selectable(label(path).c_str(), name == current))
        choose(name);
      ImGui::SetItemTooltip("%s", path.c_str());
      ImGui::PopID();
    }
    if (styles.empty())
      ImGui::TextDisabled(
          "No styles yet: right-click in Assets > New Visual Style");
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip(
      "A whole look, from a .style asset: its grading, bloom, haze, fog, "
      "ambient\nocclusion and shadows replace the scene's own, its light "
      "multiplies the "
      "scene's, and\nmaterials draw as their variants for its tag. Switch "
      "while playing with F8.");

  if (!current.empty())
  {
    if (render.visualStyle())
    {
      const std::optional<sfs::AssetId> id =
          current.rfind("asset:", 0) == 0
              ? sfs::AssetId::parse(std::string_view(current).substr(6))
              : std::nullopt;
      if (id && ImGui::Button("Edit Style"))
      {
        context.selection = {};
        context.selectedAsset = *id;
        context.revealAsset = *id;
      }
      ImGui::SameLine();
      ImGui::TextDisabled("Its look stands in for the scene's own");
    }
    else
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                         "Can't load this style: the scene looks as with none");
    return;
  }

  ImGui::PushID("grading");
  sfs::ColorGrading grading = render.colorGrading();
  if (lookWidgets::grading(&history, grading))
    render.setColorGrading(grading);
  if (!render.colorGrading().neutral() && ImGui::Button("Reset Grading"))
    history.change([&] { render.setColorGrading({}); });
  ImGui::PopID();
}

// The sky and the sun's shadow cascades (a 3D scene's). A visual style with
// its own stands in for these while it's chosen.
void sceneSkyAndShadows(sfs::RenderSystem& render, EditorHistory& history)
{
  if (render.space2D() ||
      !ImGui::CollapsingHeader("Sky & Shadows", ImGuiTreeNodeFlags_DefaultOpen))
    return;
  const sfs::VisualStyle* style = render.visualStyle();

  ImGui::PushID("sky");
  sfs::SkySettings sky = render.skySettings();
  if (lookWidgets::sky(&history, sky))
    render.setSkySettings(sky);
  if (style && style->sky)
    ImGui::TextDisabled("The visual style's sky stands in for this");
  ImGui::PopID();

  ImGui::PushID("cascades");
  sfs::ShadowSettings shadows = render.shadowSettings();
  if (lookWidgets::shadowCascades(&history, shadows))
    render.setShadowSettings(shadows);
  if (style && style->shadowCascades)
    ImGui::TextDisabled("The visual style's cascades stand in for these");
  ImGui::PopID();
}

// Ambient occlusion, contact shadows and height fog (a 3D scene's). A visual
// style with its own stands in for each while it's chosen.
void sceneAtmosphere(sfs::RenderSystem& render, EditorHistory& history)
{
  if (render.space2D() ||
      !ImGui::CollapsingHeader("Occlusion & Fog",
                               ImGuiTreeNodeFlags_DefaultOpen))
    return;
  const sfs::VisualStyle* style = render.visualStyle();

  ImGui::PushID("occlusion");
  sfs::AmbientOcclusionSettings occlusion = render.ambientOcclusionSettings();
  if (lookWidgets::ambientOcclusion(&history, occlusion))
    render.setAmbientOcclusionSettings(occlusion);
  if (style && style->ambientOcclusion)
    ImGui::TextDisabled(
        "The visual style's ambient occlusion stands in for this");
  ImGui::PopID();

  ImGui::PushID("contact");
  sfs::ContactShadowSettings contact = render.contactShadowSettings();
  if (lookWidgets::contactShadows(&history, contact))
    render.setContactShadowSettings(contact);
  if (style && style->contactShadows)
    ImGui::TextDisabled("The visual style's contact shadows stand in for this");
  ImGui::PopID();

  ImGui::PushID("fog");
  sfs::FogSettings fog = render.fogSettings();
  if (lookWidgets::fog(&history, fog))
    render.setFogSettings(fog);
  if (style && style->fog)
    ImGui::TextDisabled("The visual style's fog stands in for this");
  ImGui::PopID();
}
} // namespace

void SceneSettingsPanel::drawContents(EditorContext& context)
{
  if (!context.render || !context.scene)
    return;

  EditorHistory& history = *context.history;
  gameSystems(*context.scene, history);
  sfs::RenderSystem& render = *context.render;
  sceneStyle(render, history);
  sceneLook(context, render, history);
  sceneSkyAndShadows(render, history);
  sceneAtmosphere(render, history);
  ImGui::Separator();

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
  sfs::SceneLighting lighting = render.lighting();
  bool lightingChanged = false;
  lightingChanged |= widgets::undoable(
      &history,
      lighting.sun.direction,
      [](glm::vec3& v)
      { return ImGui::DragFloat3("Sun Direction", &v.x, 0.01f); });
  if (render.space2D())
    ImGui::SetItemTooltip("Where the sun shines from, in the game's space: x and y across\n"
                          "its plane, z up from it (toward the viewer, in a flat game)");
  // The moon is a 3D world's.
  if (!render.space2D())
  {
    lightingChanged |= widgets::undoable(
        &history,
        lighting.moon.direction,
        [](glm::vec3& v)
        { return ImGui::DragFloat3("Moon Direction", &v.x, 0.01f); });
    ImGui::SetItemTooltip("Where the moon is (z up). Once the sun has set, it lights the\n"
                          "scene and casts shadows; z below 0: no moon");
  }
  // A physical sky works the sun's colour and the sky's out itself.
  if (!render.effectiveSky().physical() || render.space2D())
  {
    lightingChanged |= widgets::undoable(
        &history,
        lighting.sun.color,
        [](glm::vec3& v) { return widgets::colorEdit3("Sun Color", &v.x); });
    if (!render.space2D())
      lightingChanged |= widgets::undoable(
          &history,
          lighting.moon.color,
          [](glm::vec3& v) { return widgets::colorEdit3("Moon Color", &v.x); });
    lightingChanged |= widgets::undoable(
        &history,
        lighting.skyColor,
        [](glm::vec3& v) { return widgets::colorEdit3("Sky Color", &v.x); });
  }
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
  const sfs::RenderStats& stats = render.stats();
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
  if (!render.space2D())
    ImGui::Text("Grass fields: %zu", stats.grassFields);
}
