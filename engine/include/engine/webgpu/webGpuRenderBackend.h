#pragma once

#include "engine/runtime/rendering/backend/iRenderBackend.h"
#include "engine/runtime/sceneManager/scene.h"
#include "engine/webgpu/webGpuContext.h"
#include "engine/webgpu/webGpuRenderTarget.h"
#include "engine/webgpu/webGpuRenderer.h"

#include <webgpu/webgpu.h>

#include <memory>
#include <optional>
#include <string>

namespace sfs
{

class AssetStore;

// WebGPU render backend: an SDL window (no GL context) plus a WebGPU device and
// surface. Each frame it opens a command encoder and backbuffer view; scenes
// draw into them through the renderer it provides (SceneServices::
// renderer). For tools it also makes render targets, preview renderers and
// images (see IRenderBackend).
//
// It also hosts Dear ImGui with docking and multi-viewport enabled: panels dock
// into the main window or drag out into their own OS windows.
class WebGpuRenderBackend : public IRenderBackend
{
public:
  WebGpuRenderBackend();
  ~WebGpuRenderBackend() override;

  std::string_view name() const override { return "webgpu"; }
  bool init(const char* title, int width, int height) override;
  SDL_Window* window() const override { return m_window; }
  void onResize(int width, int height) override;
  SceneServices* sceneServices() override;

  void beginFrame(int width, int height) override;
  void endFrame() override;

  void imguiProcessEvent(const SDL_Event& event) override;
  void imguiNewFrame() override;
  void imguiRenderDrawData() override;
  bool imguiAvailable() const override { return m_imguiReady; }
  void setVsync(bool vsync) override { m_context.setVsync(vsync); }
  bool deviceLost() const override { return m_context.deviceLost(); }

  std::unique_ptr<IRenderTarget> createRenderTarget() override;
  /** The target must be one of createRenderTarget()'s, and outlive its use. */
  void setSceneTarget(IRenderTarget* target) override;
  std::unique_ptr<IRenderer>
  createRenderer(uint32_t shadowMapResolution) override;
  void drawInto(IRenderTarget& target,
                IRenderer& renderer,
                const std::function<void()>& draw) override;
  std::unique_ptr<IImage> createImage(glm::ivec2 size,
                                      std::span<const uint8_t> rgba) override;

  void shutdown() override;

  WebGpuContext& context() { return m_context; }

private:
  bool initImGui();
  void shutdownImGui();
  void clearWindow();

  SDL_Window* m_window = nullptr;
  WebGpuContext m_context;
  int m_width = 0;
  int m_height = 0;

  WGPUSurfaceTexture m_surfTex = {};
  WGPUTextureView m_view = nullptr;
  WGPUCommandEncoder m_encoder = nullptr;

  std::unique_ptr<AssetStore> m_assetStore;
  std::unique_ptr<WebGpuRenderer> m_renderer;
  std::optional<SceneServices> m_services;

  WebGpuRenderTarget* m_sceneTarget = nullptr;

  // SFS_SCREENSHOT=<file.png>: the scene draws offscreen, and frame
  // SFS_SCREENSHOT_FRAME (60 by default) is saved there (for checking how a
  // game looks without seeing its window).
  void saveScreenshot();
  std::unique_ptr<WebGpuRenderTarget> m_screenshotTarget;
  std::string m_screenshotPath;
  int m_screenshotFrame = 60;
  int m_frame = 0;

  bool m_imguiReady = false;
  // Set when this frame drew ImGui, so endFrame renders the detached panels.
  [[maybe_unused]] bool m_imguiRendered = false; // (no ImGui on the web)
  bool m_shown = false; // the window, after the first frame
};

/** Offer WebGpuRenderBackend among RenderBackends, as "webgpu". */
void registerWebGpuBackend();

} // namespace sfs
