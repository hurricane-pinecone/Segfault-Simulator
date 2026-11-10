#include "engine/core/util/threadPriority.h"

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#elif defined(__linux__)
  #include <sys/resource.h>
  #include <sys/syscall.h>
  #include <unistd.h>
#endif

namespace sfs
{

void lowerThisThreadPriority()
{
#if defined(_WIN32)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__linux__)
  // (A thread's own niceness: Linux keeps one per thread.)
  const auto tid = static_cast<id_t>(syscall(SYS_gettid));
  setpriority(PRIO_PROCESS, tid, getpriority(PRIO_PROCESS, tid) + 5);
#endif
}

} // namespace sfs
