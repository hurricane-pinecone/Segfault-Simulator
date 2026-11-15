#pragma once

#include <webgpu/webgpu.h>

#include <cstdint>
#include <string>

namespace sfs::wgpu
{

// A WGPUStringView over a null-terminated C string (the v24 string convention).
inline WGPUStringView sv(const char* s)
{
  return WGPUStringView{s, WGPU_STRLEN};
}

/**
 * Copies a string WebGPU hands back (a message, a name) into a std::string.
 * @param s The view; a null one (no string) reads as empty.
 * @return Its characters.
 */
std::string str(WGPUStringView s);

/**
 * Compiles WGSL into a shader module.
 * @param device The device to make it on.
 * @param code The WGSL source, null-terminated.
 * @param label The module's name, which WebGPU's messages about it carry. In a
 *   browser the compiler's errors and warnings are logged under it too, with
 *   their lines in `code`.
 * @return The module. One whose source doesn't compile is still returned, but
 *   invalid: the error is raised on the device (see
 *   WebGpuContext::beginValidation()).
 */
WGPUShaderModule
makeShader(WGPUDevice device, const char* code, const char* label);

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
