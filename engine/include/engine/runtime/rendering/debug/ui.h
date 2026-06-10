#pragma once

#include "engine/runtime/rendering/backend/iRenderBackend.h"
#include "engine/runtime/rendering/modules/blockGeometry.h"
#include "engine/runtime/rendering/modules/isometricDecalSink.h"
#include "engine/runtime/rendering/modules/isometricWater.h"
#include "engine/runtime/rendering/modules/spriteShadow.h"
#include "engine/runtime/rendering/modules/terrainShadow.h"
#include "engine/runtime/rendering/util/renderStats.h"
#include "engine/runtime/sceneManager/scene.h"
#include "engine/runtime/systems/isometric/isometricRenderSystem.h"
#ifndef ENGINE_WEB
  #include "engine/core/util/allocationMetrics.h"
  #include "engine/core/util/format.h"

  #include <imgui.h>

  #include <cstdlib>
  #include <functional>
  #include <string>
  #include <typeindex>
  #include <typeinfo>
  #include <vector>
  #if defined(__GNUC__) || defined(__clang__)
    #include <cxxabi.h>
  #endif

namespace sfs
{

// Human-readable name from a mangled RTTI symbol, with the sfs:: namespace
// stripped.
inline static std::string prettyTypeName(const char* mangled)
{
  #if defined(__GNUC__) || defined(__clang__)
  int status = 0;
  char* demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
  std::string name = (status == 0 && demangled) ? demangled : mangled;
  std::free(demangled);
  #else
  std::string name = mangled;
  #endif

  const std::string prefix = "sfs::";
  if (name.rfind(prefix, 0) == 0)
    name = name.substr(prefix.size());

  return name;
}

inline static std::string prettyTypeName(const std::type_info& info)
{
  return prettyTypeName(info.name());
}

inline static void renderDebugStats()
{
  const auto& metrics = getMemoryMetrics();

  ImGui::SetNextWindowSize(ImVec2(450, 180), ImGuiCond_FirstUseEver);
  ImGui::Begin("Debug Memory");

  ImGui::Text("Current: %s", formatBytes(metrics.current.load()));
  ImGui::Text("Peak: %s", formatBytes(metrics.peak.load()));

  ImGui::Separator();

  ImGui::Text("Live allocs: %zu",
              metrics.allocationCount.load() - metrics.freeCount.load());

  ImGui::Separator();

  ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

  ImGui::Separator();

  ImGui::Text("Render Items: %d", gRenderItemCount);
  ImGui::Text("Terrain Shadow Items: %d", gTerrainShadowItems);
  ImGui::Text("Terrain shadow batches: %d", gTerrainShadowBatchCount);
  ImGui::Text("Terrain Shadow Flushes: %d", gTerrainShadowFlushes);
  ImGui::Text("Shadow edges processed: %d", gTerrainShadowEdgesProcessed);
  ImGui::Text("Tile render items: %d", gTileRenderItems);
  ImGui::Text("Sprite render items: %d", gSpriteRenderItems);
  ImGui::Text("Sprite shadows: %d", gSpriteProjectedShadowItems);
  ImGui::Text("Shadow path checks: %llu",
              static_cast<unsigned long long>(
                  gShadowPathChecks.load(std::memory_order_relaxed)));
  ImGui::Text("Shadow tiles traversed: %llu",
              static_cast<unsigned long long>(
                  gShadowTilesTraversed.load(std::memory_order_relaxed)));

  ImGui::End();
}

// Checkbox bound to a render module's presence: registering the module when
// ticked, removing it when unticked. Use only for modules that are safe to
// default-construct -- modules carrying game-side configuration (e.g. Particles
// effects) would lose it on re-creation and should not be toggled this way.
template <class TModule>
inline static void moduleCheckbox(IsometricRenderSystem& render,
                                  const char* label)
{
  bool on = render.hasModule<TModule>();
  if (ImGui::Checkbox(label, &on))
  {
    if (on)
      render.withModule<TModule>();
    else
      render.removeModule<TModule>();
  }
}

// Render one module setting descriptor with the matching ImGui control, binding
// edits straight back through its accessors. Generic over the setting type, so
// any module's settings() drive the panel without per-module UI code here.
inline static void renderModuleSetting(const ModuleSetting& setting)
{
  if (!setting.enabled)
    ImGui::BeginDisabled();

  switch (setting.type)
  {
  case ModuleSetting::Type::Float:
  {
    float value = setting.getFloat();
    if (ImGui::SliderFloat(
            setting.label.c_str(), &value, setting.min, setting.max))
      setting.setFloat(value);
    break;
  }
  case ModuleSetting::Type::Bool:
  {
    bool value = setting.getBool();
    if (ImGui::Checkbox(setting.label.c_str(), &value))
      setting.setBool(value);
    break;
  }
  case ModuleSetting::Type::Enum:
  {
    std::vector<const char*> options;
    options.reserve(setting.options.size());
    for (const auto& option : setting.options)
      options.push_back(option.c_str());

    int index = setting.getEnum();
    if (ImGui::Combo(setting.label.c_str(),
                     &index,
                     options.data(),
                     static_cast<int>(options.size())))
      setting.setEnum(index);
    break;
  }
  case ModuleSetting::Type::Action:
    if (ImGui::Button(setting.label.c_str()))
      setting.onInvoke();
    break;
  case ModuleSetting::Type::Text:
    ImGui::Text("%s: %s", setting.label.c_str(), setting.getText().c_str());
    break;
  }

  if (!setting.enabled)
    ImGui::EndDisabled();
}

// Debug-only controls: toggle each system (except the render system), enable or
// disable render modules, and tweak each module's own settings live.
inline static void renderDebugControls(Scene* scene)
{
  if (!scene)
    return;

  ImGui::SetNextWindowSize(ImVec2(300, 320), ImGuiCond_FirstUseEver);
  ImGui::Begin("Debug Systems");

  ImGui::TextUnformatted("Systems");
  ImGui::Separator();

  scene->forEachSystem(
      [](System& system)
      {
        // The render system is required for anything to draw.
        if (typeid(system) == typeid(IsometricRenderSystem))
          return;

        const std::string name = prettyTypeName(typeid(system));

        bool enabled = system.enabled();
        if (ImGui::Checkbox(name.c_str(), &enabled))
          system.setEnabled(enabled);

        // Systems expose live settings as the same UI-agnostic descriptors
        // render modules use; render them indented under the toggle.
        const std::vector<ModuleSetting> systemSettings = system.settings();
        if (!systemSettings.empty())
        {
          ImGui::PushID(name.c_str());
          ImGui::Indent();
          for (const ModuleSetting& setting : systemSettings)
            renderModuleSetting(setting);
          ImGui::Unindent();
          ImGui::PopID();
        }
      });

  if (scene->hasSystem<IsometricRenderSystem>())
  {
    auto& render = scene->getSystem<IsometricRenderSystem>();
    const bool geometryActive = render.hasModule<BlockGeometry>();

    ImGui::Separator();
    ImGui::TextUnformatted("Render Modules");

    // Block geometry and water hold no game-side configuration, so toggling
    // them just adds/removes the module (re-registration rebuilds per frame).
    moduleCheckbox<BlockGeometry>(render, "Block geometry");
    moduleCheckbox<IsometricWater>(render, "Water");

    // Decals toggle HIDES drawing rather than removing the module: the module
    // owns the accumulated stains (and the particle engine holds a raw sink
    // pointer to it), so removing it would drop the stains and dangle the sink.
    // Hiding keeps both, so the stains persist across the toggle.
    if (auto* decals = render.module<IsometricDecalSink>())
    {
      bool show = decals->visible();
      if (ImGui::Checkbox("Decals", &show))
        decals->setVisible(show);
    }

    // Sun shadows are a paired terrain+actor feature; the active technique
    // follows the render style, so the two modules toggle together.
    bool shadowsOn = render.hasModule<TerrainShadow>();
    if (ImGui::Checkbox("Shadows", &shadowsOn))
    {
      if (shadowsOn)
        render.withModules<TerrainShadow, SpriteShadow>();
      else
      {
        render.removeModule<TerrainShadow>();
        render.removeModule<SpriteShadow>();
      }
    }

    // Sun shadow style is a backend/march property (not a module setting): the
    // smooth/sharp choice only exists for the block-geometry march, so the
    // dropdown offers only what the active render style supports.
    if (render.hasModule<TerrainShadow>())
    {
      if (geometryActive)
      {
        const char* styles[] = {"Smooth", "Sharp"};
        int style = render.sunShadowStyle() == SunShadowStyle::Sharp ? 1 : 0;
        if (ImGui::Combo("Sun shadow style", &style, styles, 2))
          render.setSunShadowStyle(style == 1 ? SunShadowStyle::Sharp
                                              : SunShadowStyle::Smooth);
      }
      else
      {
        // Billboard terrain shadows are projected quads -- tile-aligned, so
        // "Sharp" is the only supported look (the march needs block geometry).
        ImGui::BeginDisabled();
        const char* projected[] = {"Sharp"};
        int style = 0;
        ImGui::Combo("Sun shadow style", &style, projected, 1);
        ImGui::EndDisabled();
      }
    }

    // Each module owns its settings as UI-agnostic descriptors; pull them in
    // and render generically. A module that exposes none is skipped. The render
    // context lets a module return only the settings that apply to the current
    // render mode.
    render.forEachModule(
        [](std::type_index type,
           IRenderModule<IsometricRenderContext>& module,
           const IsometricRenderContext& context)
        {
          const std::vector<ModuleSetting> moduleSettings =
              module.settings(context);
          if (moduleSettings.empty())
            return;

          ImGui::SeparatorText(prettyTypeName(type.name()).c_str());
          for (const ModuleSetting& setting : moduleSettings)
            renderModuleSetting(setting);
        });
  }

  ImGui::End();
}

inline static void renderDebugUI(IRenderBackend& backend,
                                 Scene* scene,
                                 const std::function<void()>& appUI = {})
{
  backend.imguiNewFrame();

  renderDebugStats();
  renderDebugControls(scene);

  // Game-specific debug controls (added by the active scene).
  if (scene)
    scene->debugUI();

  // App-level debug controls (Game::onDebugUI), drawn in the same frame.
  if (appUI)
    appUI();

  backend.imguiRenderDrawData();
}

} // namespace sfs

#endif
