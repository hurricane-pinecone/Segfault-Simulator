#pragma once

namespace sfs
{

class WebGpuContext;

// Multi-viewport rendering for Dear ImGui on WebGPU: the renderer half of
// dragging a docked panel out into its own OS window. ImGui's WebGPU backend
// doesn't implement it (only the SDL2 platform half exists), so this installs
// the Renderer_* platform hooks itself. Each secondary viewport gets a WebGPU
// surface on the SDL window the platform backend created, configured like the
// main surface, and renders and presents with its own command buffer.
//
// Call install after ImGui_ImplWGPU_Init and before the first frame; call
// shutdown before ImGui_ImplWGPU_Shutdown. The context must outlive both.
namespace imguiViewports
{

void install(WebGpuContext& context);
void shutdown();

// Render and present every secondary viewport. Call once per frame after the
// main window's frame has been submitted (each viewport submits its own
// commands, so it must not interleave with the main frame's buffer writes).
void renderPlatformWindows();

} // namespace imguiViewports
} // namespace sfs
