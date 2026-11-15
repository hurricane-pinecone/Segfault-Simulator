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

Events::~Events() = default;

void Events::update()
{
  ZoneScopedN("events/update");
  SFS_CPU_SCOPE("events/update");
  m_owner.store(detail::currentThreadId(), std::memory_order_relaxed);
  ++m_frame;
  for (const std::unique_ptr<detail::EventQueueBase>& queue : m_queues)
    if (queue->busy())
      queue->swap();
}

void Events::stats(std::vector<EventStats>& out) const
{
  out.clear();
  for (const std::unique_ptr<detail::EventQueueBase>& queue : m_queues)
    queue->stats(out.emplace_back());
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
  LOG_ERROR(std::string("Events::") + call + "<" +
            detail::readableTypeName(typeKey) +
            ">() was called off the thread that owns the events, so it was "
            "ignored. Workers send through an EventSender (Events::sender()), "
            "taken on the owning thread.");
}

} // namespace sfs
