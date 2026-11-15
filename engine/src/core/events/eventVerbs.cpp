#include "engine/core/events/eventVerbs.h"

#include "engine/core/logger/logger.h"
#include "engine/core/util/typeKey.h"

#include <string>

namespace sfs
{

bool EventVerbs::hasUnconsumedEvents() const
{
  return m_eventOwner.hasUnconsumed();
}

void EventVerbs::madeOutsideCreate(std::string_view type, const char* kind)
{
  if (m_warnedOutside)
    return;
  m_warnedOutside = true;
  LOG_ERROR("Events: " + detail::readableTypeName(typeid(*this)) + " made a " +
            kind + " of " + std::string(type) +
            " outside create(), so it receives nothing. Make it in "
            "create() and keep it as a member.");
}

void EventVerbs::noEvents() const
{
  if (m_warnedNoEvents)
    return;
  m_warnedNoEvents = true;
  LOG_ERROR("Events: " + detail::readableTypeName(typeid(*this)) +
            " used events before it had them (a system gets them as it's "
            "added to a registry), so the call did nothing.");
}

} // namespace sfs
