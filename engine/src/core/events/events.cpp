#include "engine/core/events/events.h"

#include "engine/core/logger/logger.h"
#include "engine/core/util/cpuTiming.h"
#include "engine/core/util/profiling.h"

#include "../util/typeIds.h"

#include <string>

namespace sfs
{

namespace detail
{

int eventTypeId(std::string_view typeKey)
{
  static TypeIds ids;
  return ids.idOf(typeKey);
}

} // namespace detail

Events::Events() : m_owner(detail::currentThreadId()) {}

Events::~Events()
{
  for (detail::KeyedChannel* channel : m_channels)
    channel->close();
}

void Events::update()
{
  ZoneScopedN("events/update");
  SFS_CPU_SCOPE("events/update");
  const std::uintptr_t owner = detail::currentThreadId();
  if (m_owner.load(std::memory_order_relaxed) != owner) [[unlikely]]
  {
    m_owner.store(owner, std::memory_order_relaxed);
    for (detail::KeyedChannel* channel : m_channels)
      channel->owner.store(owner, std::memory_order_relaxed);
  }
  ++m_frame;
  adoptResponses();
  for (const std::unique_ptr<detail::EventQueueBase>& queue : m_queues)
    if (queue->busy())
      queue->advance();
}

void Events::setMaxKept(std::size_t events)
{
  m_maxKept = events > 0 ? events : 1;
}

void Events::takeIn()
{
  if (!onOwnerThread()) [[unlikely]]
  {
    offThread("takeIn", "");
    return;
  }
  adoptResponses();
  for (detail::EventQueueBase* queue : m_responseQueues)
    queue->takeIn();
}

void Events::entityRemoved(Entity::EntityId id, uint32_t generation)
{
  if (!m_keyGenerations) [[unlikely]]
    makeKeyGenerations();
  if (uint32_t* least = m_keyGenerations->make(id))
    *least = generation + 1;
  for (detail::KeyedChannel* channel : m_channels)
    channel->entityRemoved(id);
}

void Events::addChannel(detail::KeyedChannel& channel)
{
  channel.owner.store(
      m_owner.load(std::memory_order_relaxed), std::memory_order_relaxed);
  channel.events = this;
  if (!m_keyGenerations)
    makeKeyGenerations();
  channel.keyGenerations = m_keyGenerations.get();
  m_channels.push_back(&channel);
}

void Events::makeKeyGenerations()
{
  m_keyGenerations = std::make_unique<detail::PagedWords<uint32_t>>();
  for (detail::KeyedChannel* channel : m_channels)
    channel->keyGenerations = m_keyGenerations.get();
}

void Events::adoptResponses()
{
  for (detail::KeyedChannel* channel : m_channels)
    if (channel->unadopted.load(std::memory_order_relaxed) &&
        channel->unadopted.exchange(false, std::memory_order_acquire))
      channel->takeUnadopted(m_adopting);
  if (m_adopting.empty()) [[likely]]
    return;
  for (void (*adopt)(Events&) : m_adopting)
    adopt(*this);
  m_adopting.clear();
}

void Events::stats(std::vector<EventStats>& out) const
{
  out.clear();
  for (const std::unique_ptr<detail::EventQueueBase>& queue : m_queues)
    queue->stats(out.emplace_back());
}

void Events::consumerStats(std::vector<EventConsumerStats>& out) const
{
  out.clear();
  for (const std::unique_ptr<detail::EventQueueBase>& queue : m_queues)
    queue->consumerStats(out);
}

detail::EventQueueBase&
Events::add(std::size_t id, std::unique_ptr<detail::EventQueueBase> queue)
{
  if (id >= m_byId.size())
    m_byId.resize(id + 1, nullptr);
  detail::EventQueueBase& made = *queue;
  m_byId[id] = &made;
  m_queues.push_back(std::move(queue));
  return made;
}

void Events::offThread(const char* call, std::string_view typeKey)
{
  const std::string type =
      typeKey.empty() ? "" : "<" + detail::readableTypeName(typeKey) + ">";
  LOG_ERROR(std::string("Events::") + call + type +
            "() was called off the thread that owns the events, so it was "
            "ignored. Workers send through an EventBroadcast "
            "(Events::broadcast()), taken on the owning thread.");
}

} // namespace sfs
