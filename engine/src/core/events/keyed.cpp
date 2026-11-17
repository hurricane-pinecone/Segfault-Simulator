#include "engine/core/events/keyed.h"

#include "engine/core/ecs/entity.h"
#include "engine/core/events/requestTickets.h"
#include "engine/core/logger/logger.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace sfs::detail
{

KeyedChannel::KeyedChannel() = default;

KeyedChannel::~KeyedChannel() = default;

void KeyedChannel::close()
{
  owner.store(0, std::memory_order_relaxed);
  events = nullptr;
  // The table goes with the events; a handle outliving them reads nothing.
  keyGenerations = nullptr;
  const std::lock_guard lock(mutex);
  open = false;

  for (ResponseSlot& slot : slots)
  {
    const std::lock_guard inboxLock(slot.inbox->mutex);
    slot.inbox->open = false;
  }
}

void KeyedChannel::entityRemoved(uint32_t id)
{
  if (tickets)
    tickets->entityRemoved(id);

  std::atomic<uint32_t>* found = m_words.find(id);

  if (!found)
    return;

  const uint32_t was = found->load(std::memory_order_relaxed);

  if (!(was & 1u))
    return;

  ++superseded;
  found->store(((was >> 1) + 1) << 1, std::memory_order_relaxed);
}

bool KeyedChannel::refuseId(uint32_t id) const
{
  if (id == Entity::InvalidId)
  {
    LOG_ERROR("Events: a keyed " + std::string(name) +
              " was dropped: it was sent about an invalid entity handle (a "
              "default sfs::Entity). Send it about the entity it's for.");
    return false;
  }

  LOG_ERROR("Events: a keyed " + std::string(name) + " for entity id " +
            std::to_string(id) + " was dropped: keyed events take ids below " +
            std::to_string(PagedWords<uint32_t>::kIds) + ".");
  return false;
}

bool KeyedChannel::refuseStale(const Entity& key)
{
  if (!m_warnedStale)
  {
    m_warnedStale = true;
    LOG_WARN("Events: a keyed " + std::string(name) + " for entity " +
             std::to_string(key.getId()) +
             " was dropped: its handle is to an entity that was destroyed, "
             "so it can't stand for the entity that has its id now.");
  }

  return false;
}

void KeyedChannel::takeUnadopted(std::vector<void (*)(Events&)>& out)
{
  const std::lock_guard lock(mutex);

  for (ResponseSlot& slot : slots)
    if (!slot.adopted && slot.adopt)
      out.push_back(slot.adopt);
}

} // namespace sfs::detail
