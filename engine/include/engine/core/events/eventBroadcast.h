#pragma once

#include "engine/core/ecs/entityDeclaration.h"
#include "engine/core/events/eventInbox.h"
#include "engine/core/events/eventQueue.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

namespace sfs
{

class Events;

/**
 * Sends events of one type from any thread: a worker's news (a level baked,
 * a search done). They wait in the queue's inbox and are taken in at the
 * next frame boundary (Events::update()), ahead of that frame's main-thread
 * events and in the order each thread sent them. Take one on the owning
 * thread with Events::broadcast<T>() and copy it into the job once (an
 * atomic increment), not per event. A send takes a mutex for one append.
 *
 * A broadcast keeps the inbox alive, not the Events: once the Events is gone,
 * send() returns false. It holds T's storage, so it mustn't outlive the code
 * that defines T: a job holding one is cancelled and joined with its scene.
 */
template <typename T>
class EventBroadcast
{
public:
  /** A broadcast to nowhere: send() always returns false. */
  EventBroadcast() = default;

  /**
   * Stages an event for the next frame.
   * @param event the event
   * @return true if it was staged; false once the Events is gone (or for a
   *         default broadcast)
   */
  bool send(const T& event) const
    requires(!detail::kKeyed<T>)
  {
    return stage(event, nullptr);
  }
  /** @copydoc send(const T&) */
  bool send(T&& event) const
    requires(!detail::kKeyed<T>)
  {
    return stage(std::move(event), nullptr);
  }

  /**
   * Stages an event of a keyed type for the next frame.
   * @param key the entity it's about
   * @param event the event
   * @return true if it was staged; false once the Events is gone (or for a
   *         default broadcast)
   */
  bool send(const Entity& key, const T& event) const
    requires detail::kKeyed<T>
  {
    return stage(event, &key);
  }
  /** @copydoc send(const Entity&, const T&) */
  bool send(const Entity& key, T&& event) const
    requires detail::kKeyed<T>
  {
    return stage(std::move(event), &key);
  }

private:
  static_assert(detail::checkEvent<T>());
  static_assert(detail::checkNotResponse<T>());
  static_assert(!detail::kRequest<T>,
                "Requests are sent on the owning thread; a worker responds "
                "through a Responder");
  friend class Events;

  explicit EventBroadcast(std::shared_ptr<detail::EventInbox<T>> inbox)
      : m_inbox(std::move(inbox))
  {
  }

  template <typename TEvent>
  bool stage(TEvent&& event, const Entity* key) const
  {
    if (!m_inbox)
      return false;

    detail::EventInbox<T>& inbox = *m_inbox;
    const std::lock_guard lock(inbox.mutex);

    if (!inbox.open)
      return false;

    inbox.staged.events.push_back(std::forward<TEvent>(event));

    if constexpr (detail::kKeyed<T>)
      inbox.staged.keys.push_back(*key);

    inbox.count.store(static_cast<uint32_t>(inbox.staged.events.size()),
                      std::memory_order_relaxed);

    if (inbox.log) [[unlikely]]
      detail::logWorkerSend(inbox.name);

    return true;
  }

  std::shared_ptr<detail::EventInbox<T>> m_inbox;
};

} // namespace sfs
