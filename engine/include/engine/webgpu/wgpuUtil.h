#pragma once

#include <webgpu/webgpu.h>

#include <cstdint>

namespace sfs::wgpu
{

// A WGPUStringView over a null-terminated C string (the v24 string convention).
inline WGPUStringView sv(const char* s)
{
  return WGPUStringView{s, WGPU_STRLEN};
}

// Compile a WGSL source string into a shader module.
inline WGPUShaderModule makeShader(WGPUDevice device, const char* code)
{
  WGPUShaderSourceWGSL wgsl = {};
  wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
  wgsl.code = sv(code);
  WGPUShaderModuleDescriptor desc = {};
  desc.nextInChain = &wgsl.chain;
  return wgpuDeviceCreateShaderModule(device, &desc);
}

inline WGPUBuffer
makeBuffer(WGPUDevice device, uint64_t size, WGPUBufferUsage usage)
{
  WGPUBufferDescriptor d = {};
  d.usage = usage;
  d.size = size;
  return wgpuDeviceCreateBuffer(device, &d);
}

// A single buffer bind-group-layout entry (binding index, stage, buffer type).
inline WGPUBindGroupLayoutEntry storageEntry(uint32_t binding,
                                             WGPUShaderStage visibility,
                                             WGPUBufferBindingType type)
{
  WGPUBindGroupLayoutEntry e = {};
  e.binding = binding;
  e.visibility = visibility;
  e.buffer.type = type;
  return e;
}

} // namespace sfs::wgpu
