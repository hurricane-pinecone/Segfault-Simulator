#pragma once

#include "engine/core/events/eventQueue.h"

#include <memory>
#include <mutex>
#include <utility>

namespace sfs
{

class Events;

/**
 * Sends events of one type from any thread: a worker's news (a plan finished,
 * a bake done). They wait in the queue's inbox and are taken in at the next
 * frame boundary (Events::update()), ahead of that frame's main-thread events
 * and in the order each thread sent them. Take one on the owning thread with
 * Events::sender<T>() and copy it into the job once (an atomic increment), not
 * per event. A send takes a mutex for one append.
 *
 * A sender keeps the inbox alive, not the Events: once the Events is gone,
 * send() returns false. It holds T's storage, so it mustn't outlive the code
 * that defines T: a job holding one is cancelled and joined with its scene.
 */
template <typename T>
class EventSender
{
public:
  /** A sender to nowhere: send() always returns false. */
  EventSender() = default;

  /**
   * Stages an event for the next frame.
   * @param event the event
   * @return true if it was staged; false once the Events is gone (or for a
   *         default sender)
   */
  bool send(const T& event) const { return stage(event); }
  /** @copydoc send(const T&) */
  bool send(T&& event) const { return stage(std::move(event)); }

private:
  friend class Events;

  explicit EventSender(std::shared_ptr<detail::EventInbox<T>> inbox)
      : m_inbox(std::move(inbox))
  {
  }

  template <typename TEvent>
  bool stage(TEvent&& event) const
  {
    if (!m_inbox)
      return false;
    detail::EventInbox<T>& inbox = *m_inbox;
    const std::lock_guard lock(inbox.mutex);
    if (!inbox.open)
      return false;
    inbox.staged.push_back(std::forward<TEvent>(event));
    inbox.count.store(
        static_cast<uint32_t>(inbox.staged.size()), std::memory_order_relaxed);
    if (inbox.log) [[unlikely]]
      detail::logWorkerSend(inbox.name);
    return true;
  }

  std::shared_ptr<detail::EventInbox<T>> m_inbox;
};

} // namespace sfs
