#include "engine/core/events/eventConsumer.h"

#include "engine/core/logger/logger.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace sfs::detail
{

void handlersWithoutOwner(std::string_view type)
{
  // Main thread only, like every consumer: no lock.
  static std::vector<std::string> logged;

  if (std::find(logged.begin(), logged.end(), type) != logged.end())
    return;

  logged.emplace_back(type);
  LOG_ERROR("Events: handlers were given to a " + std::string(type) +
            " consumer or requester with no owner, so they never run. Make "
            "it in a system's create() or an object's onCreate, and keep it "
            "as a member.");
}

} // namespace sfs::detail
