#pragma once

#include "engine/runtime/rendering/backend/iRenderBackend.h"
#include "engine/runtime/rendering/renderSystem.h"
#include "engine/runtime/sceneManager/scene.h"
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

  if (memoryTrackingAvailable())
  {
    ImGui::Text("Current: %s", formatBytes(metrics.current.load()));
    ImGui::Text("Peak: %s", formatBytes(metrics.peak.load()));

    ImGui::Separator();

    ImGui::Text("Live allocs: %zu",
                metrics.allocationCount.load() - metrics.freeCount.load());
  }
  else
  {
    ImGui::TextDisabled("Memory tracking needs a static engine build");
  }

  ImGui::Separator();

  ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

  ImGui::End();
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

// Debug-only controls: toggle each system (except the render system) and tweak
// its settings, see what the renderer drew, and tweak each render module's
// own settings live.
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
        if (typeid(system) == typeid(RenderSystem))
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

  if (auto* render = scene->tryGetSystem<RenderSystem>())
  {
    const RenderStats& stats = render->stats();
    ImGui::SeparatorText("Renderer");
    ImGui::Text("Draws: %zu  Instances: %zu", stats.drawCalls, stats.instances);
    ImGui::Text("Point lights: %zu (%zu shadowed)",
                stats.pointLights,
                stats.shadowedLights);
    for (const ModuleSetting& setting : render->settings())
      renderModuleSetting(setting);

    // Each module owns its settings as UI-agnostic descriptors; pull them in
    // and render generically. A module that exposes none is skipped.
    render->forEachModule(
        [](std::type_index type,
           IRenderModule<RenderContext>& module,
           const RenderContext& context)
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
