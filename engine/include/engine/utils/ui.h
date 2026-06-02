#pragma once

#include "engine/sceneManager/scene.h"
#include "engine/systems/isometric/isometricRenderSystem.h"
#include "engine/systems/isometric/isometricShadowSystem.h"
#include "engine/systems/isometric/isometricSpriteShadowSystem.h"
#ifndef ENGINE_WEB
  #include "imgui/backends/imgui_impl_opengl3.h"
  #include "imgui/backends/imgui_impl_sdl2.h"

  #include "allocationMetrics.h"
  #include "format.h"

  #include <imgui.h>

  #include <cstdlib>
  #include <string>
  #include <typeinfo>
  #if defined(__GNUC__) || defined(__clang__)
    #include <cxxabi.h>
  #endif

namespace sfs
{

// Human-readable system name from RTTI, with the sfs:: namespace stripped.
inline static std::string prettyTypeName(const std::type_info& info)
{
  #if defined(__GNUC__) || defined(__clang__)
  int status = 0;
  char* demangled = abi::__cxa_demangle(info.name(), nullptr, nullptr, &status);
  std::string name = (status == 0 && demangled) ? demangled : info.name();
  std::free(demangled);
  #else
  std::string name = info.name();
  #endif

  const std::string prefix = "sfs::";
  if (name.rfind(prefix, 0) == 0)
    name = name.substr(prefix.size());

  return name;
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

// Debug-only controls: toggle each system (except the render system) and tweak
// shadow settings live.
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
      });

  const bool hasTerrain = scene->hasSystem<IsometricShadowSystem>();
  const bool hasSprite = scene->hasSystem<IsometricSpriteShadowSystem>();

  if (hasTerrain || hasSprite)
  {
    ImGui::Separator();
    ImGui::TextUnformatted("Shadows");

    // One length drives both systems so terrain and sprite shadows stay tied.
    // Length/alpha feed the cached terrain geometry, so changes force a rebuild.
    float length =
        hasSprite ? scene->getSystem<IsometricSpriteShadowSystem>()
                        .shadowSettings()
                        .spriteShadowMaxLength
                  : scene->getSystem<IsometricShadowSystem>()
                        .shadowSettings()
                        .terrainShadowMaxLength;

    if (ImGui::SliderFloat("Shadow length", &length, 0.0f, 5.0f))
    {
      if (hasSprite)
        scene->getSystem<IsometricSpriteShadowSystem>()
            .shadowSettings()
            .spriteShadowMaxLength = length;

      if (hasTerrain)
      {
        auto& shadow = scene->getSystem<IsometricShadowSystem>();
        shadow.shadowSettings().terrainShadowMaxLength = length;
        shadow.markTerrainDirty();
      }
    }

    if (hasTerrain)
    {
      auto& shadow = scene->getSystem<IsometricShadowSystem>();
      if (ImGui::SliderFloat("Terrain alpha",
                             &shadow.shadowSettings().terrainShadowAlpha,
                             0.0f,
                             1.0f))
        shadow.markTerrainDirty();
    }

    if (hasSprite)
      ImGui::SliderFloat("Sprite alpha",
                         &scene->getSystem<IsometricSpriteShadowSystem>()
                              .shadowSettings()
                              .spriteShadowAlpha,
                         0.0f,
                         1.0f);
  }

  ImGui::End();
}

inline static void renderDebugUI(Scene* scene)
{
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();

  renderDebugStats();
  renderDebugControls(scene);

  // Game-specific debug controls (added by the active scene).
  if (scene)
    scene->debugUI();

  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

} // namespace sfs

#endif
