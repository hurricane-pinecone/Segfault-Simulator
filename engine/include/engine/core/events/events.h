#pragma once

#include "engine/core/events/eventQueue.h"
#include "engine/core/events/eventReader.h"
#include "engine/core/events/eventSender.h"
#include "engine/core/util/typeKey.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace sfs
{

namespace detail
{
/**
 * The event id for a type key, assigning the next free id on first use. Defined
 * once, in engine-core, like componentTypeId(), so the engine, an editor and a
 * game module agree on every event type's id.
 * @param typeKey the type's key, componentTypeKey<T>()
 * @return its id, counting from 0
 */
int eventTypeId(std::string_view typeKey);
} // namespace detail

/**
 * One registry's event queues: what one system tells others: news, notices,
 * requests and answers (docs/core/events). A sender sends a typed event; each
 * reader, made once and kept, reads every event of its type once, in its own
 * update. No callbacks, no immediate dispatch.
 *
 * Each type's queue has two buffers, swapped by update() once a frame (after
 * the flush, before the systems): an event is readable for the rest of the
 * frame it was sent in and the whole of the next, then dropped, read or not.
 * Every enabled system updates at least once a frame, so no enabled reader
 * misses one.
 *
 * Threads: the owning thread (the one that made it, rebound by each update())
 * sends, makes readers and senders, reads and updates. A send() from any other
 * thread is logged (once per type, in every build) and dropped; workers send
 * through an EventSender, whose events are taken in at the next update().
 *
 * Event types are plain structs marked [[sfs::event]]: values only (ids,
 * entities, small vectors or strings moved in), never a pointer into a
 * component or system, since they are read a frame later. A queue is made on
 * a type's first use and keeps the room of its busiest frame: nothing is
 * allocated once warm.
 */
class Events
{
public:
  Events();
  /** Closes the workers' inboxes (their senders then fail) and frees every
   * queue. */
  ~Events();
  Events(const Events&) = delete;
  Events& operator=(const Events&) = delete;

  /**
   * Sends an event: each reader of its type reads it once, in its next read()
   * (this frame or the next). On a thread other than the owner's it is
   * logged, once per type, and dropped: use a sender there.
   * @param event the event, copied or moved in
   */
  template <typename TEvent>
  void send(TEvent&& event)
  {
    using T = std::remove_cvref_t<TEvent>;
    if (!onOwnerThread()) [[unlikely]]
    {
      refuseOffThread<T>("send");
      return;
    }
    queue<T>().push(std::forward<TEvent>(event));
  }

  /**
   * A reader of TEvent, at the oldest event kept, so nothing sent while a
   * scene sets up is lost to the order things are made in. Make it once (a
   * system's create()) and keep it. On a thread other than the owner's it is
   * logged and a reader of nothing is returned.
   * @return the reader
   */
  template <typename TEvent>
  EventReader<TEvent> reader()
  {
    if (!onOwnerThread()) [[unlikely]]
    {
      refuseOffThread<TEvent>("reader");
      return {};
    }
    return EventReader<TEvent>(queue<TEvent>());
  }

  /**
   * A sender of TEvent, for a worker thread. Take it on the owning thread and
   * give it to the job; on another thread it is logged and a sender to
   * nowhere is returned.
   * @return the sender
   */
  template <typename TEvent>
  EventSender<TEvent> sender()
  {
    if (!onOwnerThread()) [[unlikely]]
    {
      refuseOffThread<TEvent>("sender");
      return {};
    }
    return EventSender<TEvent>(queue<TEvent>().inbox());
  }

  /**
   * The frame boundary: each queue drops its older frame's events, keeps
   * this frame's as the older and takes in the workers' staged ones. The
   * calling thread becomes the owner. Scene::update() calls it after the
   * flush; a headless registry's host calls it once a frame.
   */
  void update();

  /**
   * Each made queue's counts, in the order the types were first used, for the
   * debug view.
   * @param out cleared, then filled (its room is reused)
   */
  void stats(std::vector<EventStats>& out) const;

  /** How many times update() has run: the frame the log names. */
  inline uint64_t frame() const { return m_frame; }

private:
  // T's queue, made on first use.
  template <typename T>
  detail::EventQueue<T>& queue()
  {
    const auto id = static_cast<std::size_t>(
        detail::moduleTypeId<T, &detail::eventTypeId>());
    if (id < m_byId.size() && m_byId[id]) [[likely]]
      return static_cast<detail::EventQueue<T>&>(*m_byId[id]);
    return static_cast<detail::EventQueue<T>&>(
        add(id,
            std::make_unique<detail::EventQueue<T>>(
                detail::componentTypeKey<T>(), m_frame)));
  }

  // Whether the calling thread owns these events. Inline: every send asks.
  inline bool onOwnerThread() const
  {
    return detail::currentThreadId() == m_owner.load(std::memory_order_relaxed);
  }

  // Keeps a new queue under `id`.
  detail::EventQueueBase& add(std::size_t id,
                              std::unique_ptr<detail::EventQueueBase> queue);

  // A call made off the owning thread: logged once per type.
  template <typename T>
  static void refuseOffThread(const char* call)
  {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
      offThread(call, detail::componentTypeKey<T>());
  }
  static void offThread(const char* call, std::string_view typeKey);

  // By event type id: its queue, or null before its first use.
  std::vector<detail::EventQueueBase*> m_byId;
  // Every queue made, in the order made: what update() goes through.
  std::vector<std::unique_ptr<detail::EventQueueBase>> m_queues;
  // The owning thread's id. Atomic, so a thread that wrongly calls send()
  // while the owner's update() rebinds it is refused without a data race.
  std::atomic<std::uintptr_t> m_owner{0};
  uint64_t m_frame = 0;
};

} // namespace sfs
