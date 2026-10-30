#pragma once

#include "engine/runtime/rendering/backend/renderTarget.h"
#include "engine/webgpu/wgpuHandle.h"
#include "glm/glm/ext/vector_int2.hpp"

#include <webgpu/webgpu.h>

namespace sfs
{

class WebGpuContext;

/**
 * An offscreen colour texture the scene can render into instead of the window,
 * which can then be sampled, e.g. shown in an editor viewport panel with
 * ImGui::Image(target.imguiView(), ...). Same format as the window surface.
 */
class WebGpuRenderTarget final : public IRenderTarget
{
public:
  explicit WebGpuRenderTarget(WebGpuContext& context);

  /**
   * Resize to `size` pixels, recreating the texture only when it changes.
   * Non-positive sizes release it.
   */
  void resize(glm::ivec2 size) override;

  glm::ivec2 size() const override { return m_size; }
  /** Its texture (e.g. to copy what was drawn out of it). */
  WGPUTexture texture() const { return m_texture.get(); }
  /** The view the scene renders into. */
  WGPUTextureView view() const { return m_view.get(); }

  /**
   * The view to show through ImGui. On an sRGB surface ImGui's WebGPU backend
   * applies gamma (pow 2.2) to everything it draws; reading the texture without
   * sRGB decoding cancels that, so the image matches the scene drawn straight
   * to the window (as in an exported game).
   */
  WGPUTextureView imguiView() const
  {
    return m_imguiView ? m_imguiView.get() : m_view.get();
  }
  bool valid() const override { return static_cast<bool>(m_view); }
  uint64_t imguiTexture() const override
  {
    return reinterpret_cast<uintptr_t>(imguiView());
  }

private:
  WebGpuContext& m_context;
  wgpu::Texture m_texture;
  wgpu::TextureView m_view;
  wgpu::TextureView m_imguiView; // only for sRGB formats
  glm::ivec2 m_size{0, 0};
};

} // namespace sfs
