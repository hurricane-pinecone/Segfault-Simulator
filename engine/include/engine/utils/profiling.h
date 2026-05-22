#pragma once

#ifdef TRACY_ENABLE
  #include "tracy/Tracy.hpp"
#else
  #define ZoneScoped
  #define ZoneScopedN(name)
  #define FrameMark
  #define TracyPlot(name, value)
  #define TracyAlloc(memory, size)
  #define TracyFree(memory)
#endif
