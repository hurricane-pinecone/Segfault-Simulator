#include "engine/webgpu/wgpuUtil.h"

#include "engine/core/logger/logger.h"

#include <cstddef>
#include <memory>
#include <string>

namespace sfs::wgpu
{

std::string str(WGPUStringView s)
{
  if (!s.data)
    return {};

  return s.length == WGPU_STRLEN ? std::string(s.data)
                                 : std::string(s.data, s.length);
}

WGPUShaderModule
makeShader(WGPUDevice device, const char* code, const char* label)
{
  WGPUShaderSourceWGSL wgsl = {};
  wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
  wgsl.code = sv(code);
  WGPUShaderModuleDescriptor desc = {};
  desc.nextInChain = &wgsl.chain;
  desc.label = sv(label);
  const WGPUShaderModule module = wgpuDeviceCreateShaderModule(device, &desc);
#ifdef __EMSCRIPTEN__
  // The compiler's messages, each with where it is in the source. Not
  // natively: wgpu-native leaves this call unimplemented, and its validation
  // error carries the same messages with the source line.
  WGPUCompilationInfoCallbackInfo info = {};
  info.mode = WGPUCallbackMode_AllowSpontaneous;
  info.userdata1 = new std::string(label);
  info.callback = [](WGPUCompilationInfoRequestStatus status,
                     const WGPUCompilationInfo* compilation,
                     void* name,
                     void*)
  {
    const std::unique_ptr<std::string> module(static_cast<std::string*>(name));

    if (status != WGPUCompilationInfoRequestStatus_Success || !compilation)
      return;

    for (std::size_t i = 0; i < compilation->messageCount; ++i)
    {
      const WGPUCompilationMessage& message = compilation->messages[i];
      const std::string text =
          "Shader \"" + *module + "\" line " + std::to_string(message.lineNum) +
          ":" + std::to_string(message.linePos) + ": " + str(message.message);

      if (message.type == WGPUCompilationMessageType_Error)
        LOG_ERROR(text);
      else if (message.type == WGPUCompilationMessageType_Warning)
        LOG_WARN(text);
      else
        LOG_INFO(text);
    }
  };
  wgpuShaderModuleGetCompilationInfo(module, info);
#endif
  return module;
}

} // namespace sfs::wgpu
