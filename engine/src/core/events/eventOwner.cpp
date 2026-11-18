#include "engine/core/events/eventOwner.h"

#include "engine/core/events/eventQueue.h"
#include "engine/core/logger/logger.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace sfs::detail
{

EventUser::~EventUser() = default;

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

namespace
{

// The generation of the slot a link and slot name, or 0 once its queue is
// gone.
uint32_t generationOf(const EventQueueLink* link, uint32_t slot)
{
  const EventQueueBase* queue = link ? link->queue : nullptr;
  return queue ? queue->consumer(slot).generation : 0u;
}

// Whether the consumer a link, slot and generation name is still the slot's.
bool current(const EventQueueLink* link, uint32_t slot, uint32_t generation)
{
  const EventQueueBase* queue = link ? link->queue : nullptr;

  if (!queue)
    return false;

  const ConsumerSlot& self = queue->consumer(slot);
  return self.live && self.generation == generation;
}

// Clears the owner's consumers that went, and their handlers, once they're
// half of its consumers: each consumer going costs the same, however many.
void clearGone(EventOwner& owner)
{
  if (owner.forgotten == 0u || 2u * owner.forgotten < owner.consumers.size())
    return;

  owner.forgotten = 0;
  std::erase_if(owner.consumers,
                [](const ConsumerRef& ref)
                { return !current(ref.link.get(), ref.slot, ref.generation); });
  std::erase_if(owner.handlers,
                [](const EventHandler& handler) {
                  return !current(
                      handler.link.get(), handler.slot, handler.generation);
                });
}

} // namespace

void EventOwner::add(const std::shared_ptr<EventQueueLink>& link, uint32_t slot)
{
  consumers.push_back({link, slot, generationOf(link.get(), slot)});
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

  handler.generation = generationOf(handler.link.get(), handler.slot);
  handlers.push_back(std::move(handler));
}

void EventOwner::begin()
{
  ++stamp;
  updating = true;
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

    if (!self.live || self.generation != handler.generation)
      continue;

    // Nothing new, nor in this update's span already fixed: not run.
    const bool spanFixed = self.spanStamp == stamp && updating;

    if (!queue->hasNew(handler.slot) &&
        (!spanFixed || self.spanBegin == self.spanEnd))
      continue;

    handler.run(*queue, handler.slot);
  }

  dispatching = false;
  clearGone(*this);
}

void EventOwner::end()
{
  // A call between updates (a hook, render(), a direct update()) gets a span
  // of its own, from where the consumer has read to, not this one's.
  ++stamp;
  updating = false;
  wasDisabled = false;
}

void EventOwner::forget()
{
  ++forgotten;

  if (!dispatching)
    clearGone(*this);
}

bool EventOwner::hasUnconsumed() const
{
  for (const ConsumerRef& ref : consumers)
  {
    if (current(ref.link.get(), ref.slot, ref.generation) &&
        ref.link->queue->hasNew(ref.slot))
      return true;
  }

  return false;
}

} // namespace sfs::detail
