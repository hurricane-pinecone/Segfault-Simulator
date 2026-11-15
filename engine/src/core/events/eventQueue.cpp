#include "engine/core/events/eventQueue.h"

#include "engine/core/logger/logger.h"
#include "engine/core/util/debugFeatures.h"
#include "engine/core/util/typeKey.h"

#include <string>
#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

namespace sfs::detail
{

#if defined(_WIN32)
std::uintptr_t currentThreadId()
{
  return static_cast<std::uintptr_t>(GetCurrentThreadId());
}
#endif

namespace
{

// A consumer, for people: its owner's name, or "a consumer".
std::string consumerName(const ConsumerSlot* consumer)
{
  return consumer ? consumer->name : std::string("nothing");
}

} // namespace

void logWorkerSend(std::string_view type)
{
  LOG_INFO("Event " + std::string(type) + " sent by thread " +
           std::to_string(currentThreadId()) + ", for the next frame");
}

EventQueueBase::EventQueueBase(std::string name,
                               const uint64_t& frame,
                               const std::size_t& maxKept,
                               std::string kind)
    : m_link(std::make_shared<EventQueueLink>(EventQueueLink{this})),
      m_name(std::move(name)), m_kind(std::move(kind)),
      m_log(debugFeatures::logEvents(m_name)), m_frame(frame),
      m_maxKept(maxKept)
{
}

EventQueueBase::~EventQueueBase() { m_link->queue = nullptr; }

uint32_t EventQueueBase::addConsumer(EventOwner* owner,
                                     const std::type_info* who)
{
  uint32_t slot = 0;
  while (slot < m_consumers.size() && m_consumers[slot].live)
    ++slot;
  if (slot == m_consumers.size())
    m_consumers.emplace_back();
  ConsumerSlot& consumer = m_consumers[slot];
  consumer = ConsumerSlot{};
  consumer.next = m_oldestId;
  consumer.scanned = m_oldestId;
  consumer.spanBegin = consumer.spanEnd = m_oldestId;
  consumer.owner = owner;
  if (owner)
    consumer.spanStamp = owner->stamp - 1u;
  consumer.name = who ? readableTypeName(*who) : "a consumer";
  consumer.live = true;
  ++m_liveConsumers;
  return slot;
}

void EventQueueBase::removeConsumer(uint32_t slot)
{
  if (slot >= m_consumers.size() || !m_consumers[slot].live)
    return;
  m_consumers[slot].live = false;
  --m_liveConsumers;
}

void EventQueueBase::newSpan(ConsumerSlot& self)
{
#ifndef NDEBUG
  if (self.owner && !self.owner->updating) [[unlikely]]
    self.owner->consumedOutside(m_name, self.name);
#endif
  if (self.next < m_oldestId) [[unlikely]]
  {
    missed(m_oldestId - self.next, self, self.owner && self.owner->wasDisabled);
    self.next = m_oldestId;
  }
  self.spanBegin = self.next;
  self.spanEnd = m_endId;
  self.next = m_endId;
  self.spanStamp = self.owner ? self.owner->stamp : 0u;
}

void EventQueueBase::consumerStats(std::vector<EventConsumerStats>& out) const
{
  for (const ConsumerSlot& consumer : m_consumers)
  {
    if (!consumer.live)
      continue;
    EventConsumerStats& row = out.emplace_back();
    row.type = m_name;
    row.consumer = consumer.name;
    row.how = consumer.consuming == Consuming::Latest ? "readLatest"
              : consumer.consuming == Consuming::All  ? "read"
                                                      : "";
    const uint64_t from =
        consumer.next < m_oldestId ? m_oldestId : consumer.next;
    row.unconsumed = static_cast<std::size_t>(m_endId - from);
  }
}

void EventQueueBase::logSent(uint64_t id) const
{
  LOG_INFO("Event " + m_name + " [" + m_kind + "] #" + std::to_string(id) +
           " sent in frame " + std::to_string(m_frame));
}

void EventQueueBase::logSent(uint64_t id,
                             const Entity& key,
                             uint32_t generation) const
{
  std::string line = "Event " + m_name + " [" + m_kind + "] #" +
                     std::to_string(id) + " for entity " +
                     std::to_string(key.getId());
  if (generation != 0)
    line += " (its number " + std::to_string(generation) + ")";
  LOG_INFO(line + " sent in frame " + std::to_string(m_frame));
}

void EventQueueBase::logCancelled(const Entity& key, uint32_t generation) const
{
  LOG_INFO("Event " + m_name + " for entity " + std::to_string(key.getId()) +
           " cancelled (its number " + std::to_string(generation) +
           ") in frame " + std::to_string(m_frame));
}

void EventQueueBase::logDropped(const Entity& key, uint32_t generation) const
{
  LOG_INFO("Event " + m_name + " for entity " + std::to_string(key.getId()) +
           " dropped: it responds to request number " +
           std::to_string(generation) + ", which was superseded");
}

void EventQueueBase::logTaken(uint64_t first, std::size_t count) const
{
  LOG_INFO("Events " + m_name + " #" + std::to_string(first) + " to #" +
           std::to_string(first + count - 1) +
           " taken in from workers in frame " + std::to_string(m_frame));
}

void EventQueueBase::warnMany(std::size_t kept, const ConsumerSlot* holder)
{
  m_warnedMany = true;
  LOG_WARN("Events: " + std::to_string(kept) + " " + m_name +
           " events are kept, held by " + consumerName(holder) +
           ", which hasn't read them. Does it read them each update, or is "
           "something sending them in a loop?");
}

void EventQueueBase::overCap(uint64_t dropped, const ConsumerSlot* holder)
{
  if (m_warnedCap)
    return;
  m_warnedCap = true;
  LOG_ERROR("Events: " + m_name + " kept more than " +
            std::to_string(m_maxKept) +
            " events (Events::setMaxKept()), so its oldest " +
            std::to_string(dropped) + " were dropped. They were held by " +
            consumerName(holder) + ", which hasn't read them for a long time.");
}

void EventQueueBase::missed(uint64_t count,
                            ConsumerSlot& consumer,
                            bool disabled)
{
  m_missed += count;
  if (consumer.missedLogged)
    return;
  consumer.missedLogged = true;
  LOG_WARN(
      "Events: " + consumerName(&consumer) + " missed " +
      std::to_string(count) + " " + m_name +
      " events, dropped at the kept cap before it read them" +
      (disabled ? std::string(" while it was disabled.") : std::string(".")));
}

void EventQueueBase::routeGrows(std::string_view requester, std::size_t count)
{
  LOG_WARN("Events: " + std::string(requester) + "'s requester has " +
           std::to_string(count) + " " + m_name +
           " responses it hasn't read. Does its owner run? The kept cap "
           "drops the oldest.");
}

void EventQueueBase::keptRange()
{
  if (!m_warnedKeptRange)
  {
    m_warnedKeptRange = true;
    const std::string reader =
        m_lastReader < m_consumers.size()
            ? " (likely " + m_consumers[m_lastReader].name + "'s)"
            : std::string();
    LOG_WARN("Events: a range of " + m_name + " events" + reader +
             " was kept past its frame, so " + m_name +
             " is held back from every consumer until it goes. Use a range "
             "in the loop or handler that gets it.");
  }
  if (m_pendingCount >= kManyEvents && !m_warnedPendingMany)
  {
    m_warnedPendingMany = true;
    LOG_WARN("Events: " + std::to_string(m_pendingCount) + " " + m_name +
             " events wait behind a range kept past its frame.");
  }
}

void EventQueueBase::pendingOverCap(std::size_t dropped)
{
  // Missed counts over every consumer, as the buffer's drops do.
  m_missed += dropped * m_liveConsumers;
  if (m_warnedCap)
    return;
  m_warnedCap = true;
  LOG_ERROR("Events: " + m_name + " kept more than " +
            std::to_string(m_maxKept) +
            " events behind a range kept past its frame, so its oldest " +
            std::to_string(dropped) + " were dropped.");
}

} // namespace sfs::detail
