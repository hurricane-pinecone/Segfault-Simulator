#include "absentBackend.h"

#include "engine/core/logger/logger.h"

#include <atomic>
#include <string>
#include <string_view>

namespace sfs::facility
{

void warnAbsent(std::atomic<bool>& warned,
                std::string_view accessor,
                std::string_view backend)
{
  if (warned.load(std::memory_order_relaxed) ||
      warned.exchange(true, std::memory_order_relaxed))
    return;

  LOG_WARN(std::string(accessor) + "() used where there's no " +
           std::string(backend) +
           ": its commands do nothing and its queries give empty answers");
}

} // namespace sfs::facility
