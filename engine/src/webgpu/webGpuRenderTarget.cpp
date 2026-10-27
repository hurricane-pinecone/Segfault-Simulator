#include "engine/webgpu/webGpuRenderTarget.h"

#include "engine/webgpu/webGpuContext.h"

namespace sfs
{

WebGpuRenderTarget::WebGpuRenderTarget(WebGpuContext& context)
    : m_context(context)
{
}

void WebGpuRenderTarget::resize(glm::ivec2 size)
{
  if (size == m_size && valid())
    return;

  m_view.reset();
  m_texture.reset();
  m_size = size;
  if (size.x <= 0 || size.y <= 0)
    return;

  WGPUTextureDescriptor desc = {};
  desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
  desc.dimension = WGPUTextureDimension_2D;
  desc.size = {static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), 1};
  desc.format = m_context.format();
  desc.mipLevelCount = 1;
  desc.sampleCount = 1;

  m_texture.reset(wgpuDeviceCreateTexture(m_context.device(), &desc));
  m_view.reset(wgpuTextureCreateView(m_texture.get(), nullptr));
}

} // namespace sfs
