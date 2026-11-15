#include "engine/core/events/eventOwner.h"

#include "engine/core/events/eventQueue.h"
#include "engine/core/logger/logger.h"

#include <algorithm>
#include <string>

namespace sfs::detail
{

EventOwner::~EventOwner()
{
  for (const ConsumerRef& ref : consumers)
    if (EventQueueBase* queue = ref.link ? ref.link->queue : nullptr)
    {
      ConsumerSlot& slot = queue->consumer(ref.slot);
      if (slot.owner == this)
        slot.owner = nullptr;
    }
}

void EventOwner::add(const std::shared_ptr<EventQueueLink>& link, uint32_t slot)
{
  consumers.push_back({link, slot});
}

void EventOwner::forget(const EventQueueLink* link, uint32_t slot)
{
  std::erase_if(consumers,
                [&](const ConsumerRef& ref)
                { return ref.link.get() == link && ref.slot == slot; });
  if (dispatching)
  {
    // The handler running may be one of these: they're dropped after.
    for (EventHandler& handler : handlers)
      if (handler.link.get() == link && handler.slot == slot)
        handler.link.reset();
    forgotDuringDispatch = true;
    return;
  }
  std::erase_if(handlers,
                [&](const EventHandler& handler)
                { return handler.link.get() == link && handler.slot == slot; });
}

void EventOwner::addHandler(EventHandler handler)
{
  if (dispatching) [[unlikely]]
  {
    if (!warnedLateHandler)
    {
      warnedLateHandler = true;
      LOG_ERROR("Events: a handler was given to a consumer while its owner's "
                "handlers ran, so it was ignored. Give handlers as the "
                "consumer is made, in create() or onCreate.");
    }
    return;
  }
  handlers.push_back(std::move(handler));
}

void EventOwner::begin()
{
  ++stamp;
  updating = true;
}

void EventOwner::end()
{
  // A call between updates (a hook, render(), a direct update()) gets a span
  // of its own, from where the consumer has read to, not this one's.
  ++stamp;
  updating = false;
  wasDisabled = false;
}

void EventOwner::dispatch()
{
  dispatching = true;
  for (std::size_t i = 0; i < handlers.size(); ++i)
  {
    EventHandler& handler = handlers[i];
    EventQueueBase* queue = handler.link ? handler.link->queue : nullptr;
    if (!queue)
      continue;
    const ConsumerSlot& self = queue->consumer(handler.slot);
    // Nothing new, nor in this update's span already fixed: not run.
    const bool spanFixed = self.spanStamp == stamp && updating;
    if (!queue->hasNew(handler.slot) &&
        (!spanFixed || self.spanBegin == self.spanEnd))
      continue;
    handler.run(*queue, handler.slot);
  }
  dispatching = false;
  if (forgotDuringDispatch) [[unlikely]]
  {
    forgotDuringDispatch = false;
    std::erase_if(
        handlers, [](const EventHandler& handler) { return !handler.link; });
  }
}

bool EventOwner::hasUnconsumed() const
{
  for (const ConsumerRef& ref : consumers)
  {
    const EventQueueBase* queue = ref.link ? ref.link->queue : nullptr;
    if (queue && queue->hasNew(ref.slot))
      return true;
  }
  return false;
}

void EventOwner::consumedOutside(std::string_view type, std::string_view who)
{
  if (warnedOutside)
    return;
  warnedOutside = true;
  LOG_ERROR("Events: " + std::string(who) + " read " + std::string(type) +
            " events outside its update (in a hook, a callback, render() or "
            "create()). That read gets what arrived since its last; read in "
            "update(), or give the consumer a handler.");
}

} // namespace sfs::detail
