#pragma once

// GPU (OpenGL) Tracy zones. Include AFTER the OpenGL loader (GLEW / GLES3) in
// translation units that own the GL context. No-ops unless TRACY_ENABLE, so
// Debug and web builds strip them.
#ifdef TRACY_ENABLE
  #include "tracy/TracyOpenGL.hpp"
#else
  #define TracyGpuContext
  #define TracyGpuContextName(name, size)
  #define TracyGpuZone(name)
  #define TracyGpuCollect
#endif
