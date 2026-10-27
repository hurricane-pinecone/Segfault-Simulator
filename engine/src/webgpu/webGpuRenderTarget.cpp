#include "engine/webgpu/webGpuRenderTarget.h"

#include "engine/webgpu/webGpuContext.h"

namespace sfs
{

namespace
{
// The same format without sRGB encoding (itself if it has none).
WGPUTextureFormat withoutSrgb(WGPUTextureFormat format)
{
  switch (format)
  {
  case WGPUTextureFormat_BGRA8UnormSrgb:
    return WGPUTextureFormat_BGRA8Unorm;
  case WGPUTextureFormat_RGBA8UnormSrgb:
    return WGPUTextureFormat_RGBA8Unorm;
  default:
    return format;
  }
}
} // namespace

WebGpuRenderTarget::WebGpuRenderTarget(WebGpuContext& context)
    : m_context(context)
{
}

void WebGpuRenderTarget::resize(glm::ivec2 size)
{
  if (size == m_size && valid())
    return;

  m_imguiView.reset();
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
  const WGPUTextureFormat linear = withoutSrgb(desc.format);
  if (linear != desc.format)
  {
    desc.viewFormatCount = 1;
    desc.viewFormats = &linear;
  }

  m_texture.reset(wgpuDeviceCreateTexture(m_context.device(), &desc));
  m_view.reset(wgpuTextureCreateView(m_texture.get(), nullptr));
  if (linear != desc.format)
  {
    WGPUTextureViewDescriptor viewDesc = {};
    viewDesc.format = linear;
    viewDesc.dimension = WGPUTextureViewDimension_2D;
    viewDesc.mipLevelCount = 1;
    viewDesc.arrayLayerCount = 1;
    viewDesc.aspect = WGPUTextureAspect_All;
    m_imguiView.reset(wgpuTextureCreateView(m_texture.get(), &viewDesc));
  }
}

} // namespace sfs
