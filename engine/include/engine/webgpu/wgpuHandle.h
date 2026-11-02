#pragma once

#include <webgpu/webgpu.h>

#include <utility>

namespace sfs::wgpu
{

/**
 * Owning wrapper for a WebGPU object handle: releases it on destruction, moves
 * but never copies. `Releaser` is a function object rather than a function
 * pointer, because the release entry points are DLL imports on Windows and
 * can't be template arguments there.
 */
template <typename T, typename Releaser>
class Handle
{
public:
  Handle() = default;
  explicit Handle(T handle) : m_handle(handle) {}
  ~Handle() { reset(); }

  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;

  Handle(Handle&& other) noexcept
      : m_handle(std::exchange(other.m_handle, nullptr))
  {
  }

  Handle& operator=(Handle&& other) noexcept
  {
    if (this != &other)
      reset(std::exchange(other.m_handle, nullptr));
    return *this;
  }

  /** Release the current object (if any) and take ownership of `handle`. */
  void reset(T handle = nullptr)
  {
    if (m_handle)
      Releaser{}(m_handle);
    m_handle = handle;
  }

  T get() const { return m_handle; }
  explicit operator bool() const { return m_handle != nullptr; }

private:
  T m_handle = nullptr;
};

#define SFS_WGPU_HANDLE(Name, Type, ReleaseFn)                                 \
  struct Name##Releaser                                                        \
  {                                                                            \
    void operator()(Type handle) const { ReleaseFn(handle); }                  \
  };                                                                           \
  using Name = Handle<Type, Name##Releaser>;

SFS_WGPU_HANDLE(Buffer, WGPUBuffer, wgpuBufferRelease)
SFS_WGPU_HANDLE(Texture, WGPUTexture, wgpuTextureRelease)
SFS_WGPU_HANDLE(TextureView, WGPUTextureView, wgpuTextureViewRelease)
SFS_WGPU_HANDLE(Sampler, WGPUSampler, wgpuSamplerRelease)
SFS_WGPU_HANDLE(ShaderModule, WGPUShaderModule, wgpuShaderModuleRelease)
SFS_WGPU_HANDLE(BindGroupLayout,
                WGPUBindGroupLayout,
                wgpuBindGroupLayoutRelease)
SFS_WGPU_HANDLE(BindGroup, WGPUBindGroup, wgpuBindGroupRelease)
SFS_WGPU_HANDLE(PipelineLayout, WGPUPipelineLayout, wgpuPipelineLayoutRelease)
SFS_WGPU_HANDLE(RenderPipeline, WGPURenderPipeline, wgpuRenderPipelineRelease)
SFS_WGPU_HANDLE(ComputePipeline,
                WGPUComputePipeline,
                wgpuComputePipelineRelease)
SFS_WGPU_HANDLE(QuerySet, WGPUQuerySet, wgpuQuerySetRelease)

#undef SFS_WGPU_HANDLE

} // namespace sfs::wgpu
