#include "webgpu/imgui/imguiViewportRenderer.h"

#include "engine/core/logger/logger.h"
#include "engine/webgpu/webGpuContext.h"

#include "imgui.h"
#include "imgui/backends/imgui_impl_wgpu.h"

#include <SDL.h>
#include <sdl2webgpu.h>

namespace sfs::imguiViewports
{

namespace
{

// ImGui's platform hooks are plain function pointers with no user argument,
// so the context they render with is held here between install/shutdown.
WebGpuContext* g_context = nullptr;

// Per-viewport renderer state, stored in ImGuiViewport::RendererUserData.
struct ViewportSurface
{
  WGPUSurface surface = nullptr;
  WGPUSurfaceTexture frame =
      {}; // acquired in RenderWindow, presented in SwapBuffers
  int width = 0;
  int height = 0;
};

ViewportSurface* surfaceOf(ImGuiViewport* viewport)
{
  return static_cast<ViewportSurface*>(viewport->RendererUserData);
}

void configure(ViewportSurface& target, const ImVec2& size)
{
  target.width = static_cast<int>(size.x);
  target.height = static_cast<int>(size.y);
  if (target.width > 0 && target.height > 0)
    g_context->configureSurface(target.surface, target.width, target.height);
}

void createWindow(ImGuiViewport* viewport)
{
  // The SDL2 platform backend stores the window's ID, not the window.
  SDL_Window* window = SDL_GetWindowFromID(static_cast<Uint32>(
      reinterpret_cast<intptr_t>(viewport->PlatformHandle)));
  if (!window)
  {
    LOG_ERROR("ImGui viewport: no SDL window for the new platform window");
    return;
  }

  auto* target = IM_NEW(ViewportSurface)();
  target->surface = SDL_GetWGPUSurface(g_context->instance(), window);
  configure(*target, viewport->Size);
  viewport->RendererUserData = target;
}

void destroyWindow(ImGuiViewport* viewport)
{
  if (ViewportSurface* target = surfaceOf(viewport))
  {
    if (target->frame.texture)
      wgpuTextureRelease(target->frame.texture);
    if (target->surface)
      wgpuSurfaceRelease(target->surface);
    IM_DELETE(target);
  }
  viewport->RendererUserData = nullptr;
}

void setWindowSize(ImGuiViewport* viewport, ImVec2 size)
{
  if (ViewportSurface* target = surfaceOf(viewport))
    configure(*target, size);
}

void renderWindow(ImGuiViewport* viewport, void*)
{
  ViewportSurface* target = surfaceOf(viewport);
  if (!target || target->width <= 0 || target->height <= 0)
    return;

  wgpuSurfaceGetCurrentTexture(target->surface, &target->frame);
  if (target->frame.status !=
          WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
      target->frame.status !=
          WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)
  {
    // Lost or outdated (e.g. mid-resize): reconfigure and try next frame.
    if (target->frame.texture)
      wgpuTextureRelease(target->frame.texture);
    target->frame = {};
    configure(*target, viewport->Size);
    return;
  }

  const WGPUDevice device = g_context->device();
  WGPUTextureView view = g_context->surfaceView(target->frame.texture);
  WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);

  WGPURenderPassColorAttachment color = {};
  color.view = view;
  color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  color.loadOp = (viewport->Flags & ImGuiViewportFlags_NoRendererClear)
                     ? WGPULoadOp_Load
                     : WGPULoadOp_Clear;
  color.storeOp = WGPUStoreOp_Store;
  color.clearValue = WGPUColor{0.0, 0.0, 0.0, 1.0};

  WGPURenderPassDescriptor passDesc = {};
  passDesc.colorAttachmentCount = 1;
  passDesc.colorAttachments = &color;

  WGPURenderPassEncoder pass =
      wgpuCommandEncoderBeginRenderPass(encoder, &passDesc);
  ImGui_ImplWGPU_RenderDrawData(viewport->DrawData, pass);
  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);

  WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
  wgpuQueueSubmit(g_context->queue(), 1, &commands);

  wgpuCommandBufferRelease(commands);
  wgpuCommandEncoderRelease(encoder);
  wgpuTextureViewRelease(view);
}

void swapBuffers(ImGuiViewport* viewport, void*)
{
  ViewportSurface* target = surfaceOf(viewport);
  if (!target || !target->frame.texture)
    return;

  wgpuSurfacePresent(target->surface);
  wgpuTextureRelease(target->frame.texture);
  target->frame = {};
}

} // namespace

void install(WebGpuContext& context)
{
  g_context = &context;

  ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
  platform.Renderer_CreateWindow = createWindow;
  platform.Renderer_DestroyWindow = destroyWindow;
  platform.Renderer_SetWindowSize = setWindowSize;
  platform.Renderer_RenderWindow = renderWindow;
  platform.Renderer_SwapBuffers = swapBuffers;

  ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
}

void shutdown()
{
  // Destroys every secondary viewport's window (and our surface with it).
  ImGui::DestroyPlatformWindows();

  ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
  platform.Renderer_CreateWindow = nullptr;
  platform.Renderer_DestroyWindow = nullptr;
  platform.Renderer_SetWindowSize = nullptr;
  platform.Renderer_RenderWindow = nullptr;
  platform.Renderer_SwapBuffers = nullptr;

  ImGui::GetIO().BackendFlags &= ~ImGuiBackendFlags_RendererHasViewports;
  g_context = nullptr;
}

void renderPlatformWindows()
{
  if (!(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable))
    return;

  ImGui::UpdatePlatformWindows();
  ImGui::RenderPlatformWindowsDefault();
}

} // namespace sfs::imguiViewports
