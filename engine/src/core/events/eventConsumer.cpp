#include "engine/core/events/eventConsumer.h"

#include "engine/core/events/eventOwner.h"
#include "engine/core/logger/logger.h"
#include "engine/core/util/typeKey.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <typeinfo>
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

void handlerOfOtherClass(std::string_view type,
                         const std::type_info& handlerClass,
                         const EventUser* owner)
{
  const std::string ownerName = owner ? readableTypeName(typeid(*owner))
                                      : std::string("no system, service or "
                                                    "object");
  LOG_ERROR("Events: a handler given to a " + std::string(type) +
            " consumer or requester is a member function of " +
            readableTypeName(handlerClass) + ", but the handle's owner is " +
            ownerName +
            ", so it never runs. Give it a member function of "
            "the class that makes the handle.");
}

} // namespace sfs::detail
