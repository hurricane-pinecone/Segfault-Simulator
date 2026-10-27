#pragma once

#include "engine/webgpu/wgpuHandle.h"
#include "glm/glm/ext/vector_int2.hpp"

#include <webgpu/webgpu.h>

namespace sfs
{

class WebGpuContext;

/**
 * An offscreen colour texture the scene can render into instead of the window,
 * which can then be sampled, e.g. shown in an editor viewport panel with
 * ImGui::Image(target.view(), ...). Same format as the window surface.
 */
class WebGpuRenderTarget
{
public:
  explicit WebGpuRenderTarget(WebGpuContext& context);

  /**
   * Resize to `size` pixels, recreating the texture only when it changes.
   * Non-positive sizes release it.
   */
  void resize(glm::ivec2 size);

  glm::ivec2 size() const { return m_size; }
  WGPUTextureView view() const { return m_view.get(); }
  bool valid() const { return static_cast<bool>(m_view); }

private:
  WebGpuContext& m_context;
  wgpu::Texture m_texture;
  wgpu::TextureView m_view;
  glm::ivec2 m_size{0, 0};
};

} // namespace sfs
