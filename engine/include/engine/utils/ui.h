#pragma once

#include "engine/systems/isometric/isometricRenderSystem.h"
#include "engine/systems/isometric/isometricShadowSystem.h"
#ifndef ENGINE_WEB
  #include "imgui/backends/imgui_impl_opengl3.h"
  #include "imgui/backends/imgui_impl_sdl2.h"

  #include "allocationMetrics.h"
  #include "format.h"

  #include <imgui.h>

namespace sfs
{

inline static void renderDebugUI()
{
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();

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

  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

} // namespace sfs

#endif
