#pragma once

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

  ImGui::End();

  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

} // namespace sfs

#endif
