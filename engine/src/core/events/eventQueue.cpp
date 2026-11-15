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

void logWorkerSend(std::string_view type)
{
  LOG_INFO("Event " + std::string(type) + " sent by thread " +
           std::to_string(currentThreadId()) + ", for the next frame");
}

EventQueueBase::EventQueueBase(std::string_view typeKey, const uint64_t& frame)
    : m_name(readableTypeName(typeKey)),
      m_log(debugFeatures::logEvents(m_name)), m_frame(frame)
{
}

void EventQueueBase::logSent(uint64_t id) const
{
  LOG_INFO("Event " + m_name + " #" + std::to_string(id) + " sent in frame " +
           std::to_string(m_frame));
}

void EventQueueBase::logTaken(uint64_t first, std::size_t count) const
{
  LOG_INFO("Events " + m_name + " #" + std::to_string(first) + " to #" +
           std::to_string(first + count - 1) +
           " taken in from workers in frame " + std::to_string(m_frame));
}

void EventQueueBase::warnMany(std::size_t kept)
{
  m_warnedMany = true;
  if (debugFeatures::enabled())
    LOG_WARN("Events: " + std::to_string(kept) + " " + m_name +
             " events are kept. Is something sending them in a loop, or is "
             "the scene not being updated?");
}

void EventQueueBase::missed(uint64_t count, bool& logged)
{
  m_missed += count;
  if (logged)
    return;
  logged = true;
  LOG_WARN("Events: a reader of " + m_name + " missed " +
           std::to_string(count) +
           " events, dropped before it read them. Read every reader each "
           "update, before any early return, from a reader made once and "
           "kept.");
}

void EventQueueBase::sentWhileRead()
{
  if (m_warnedSentWhileRead)
    return;
  m_warnedSentWhileRead = true;
  LOG_ERROR("Events: a " + m_name + " was sent while a loop went through " +
            m_name +
            " events, so the loop was ended. Collect what to send, "
            "then send it after the loop.");
}

} // namespace sfs::detail
