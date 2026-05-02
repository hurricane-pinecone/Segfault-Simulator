#pragma once

#include "allocationMetrics.h"
#include "format.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include <SDL_render.h>

namespace sfs
{

inline static void renderDebugUI(SDL_Renderer* renderer)
{
  ImGui_ImplSDLRenderer2_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();

  const auto& metrics = getMemoryMetrics();

  ImGui::SetNextWindowSize(ImVec2(450, 180), ImGuiCond_FirstUseEver);
  ImGui::Begin("Debug Memory");

  ImGui::Text("Current: %s", formatBytes(metrics.current.load()));
  ImGui::Text("Peak: %s", formatBytes(metrics.peak.load()));
  ImGui::Text("Allocated: %s", formatBytes(metrics.allocated.load()));
  ImGui::Text("Freed: %s", formatBytes(metrics.freed.load()));
  ImGui::Separator();
  ImGui::Text("Alloc count: %zu", metrics.allocationCount.load());
  ImGui::Text("Free count: %zu", metrics.freeCount.load());
  ImGui::Text("Live allocs: %zu",
              metrics.allocationCount.load() - metrics.freeCount.load());

  ImGui::Separator();
  ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

  ImGui::End();

  ImGui::Render();
  ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
}

} // namespace sfs
