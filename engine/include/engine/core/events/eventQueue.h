#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#if !defined(_WIN32)
  #include <pthread.h>
#endif

// One event type's queue: what Events keeps per type, and what its readers and
// senders point at. See events.h for how events are sent and read.
namespace sfs
{

/** One event type's counts, for the debug view (Events::stats()). */
struct EventStats
{
  /**
   * The event type's name, for people ("FieldCellChanged",
   * "sfs::nav::AgentEvent"); valid while its Events lives.
   */
  std::string_view type;
  /**
   * Events sent in the last whole frame: the main thread's, and the workers'
   * taken in as it began.
   */
  std::size_t sentLastFrame = 0;
  /** Events kept now (the last frame's and this one's so far). */
  std::size_t kept = 0;
  /** Workers' events waiting for the next frame. */
  std::size_t staged = 0;
  /** Room in the queue's two buffers, in events: the busiest frame's. */
  std::size_t capacity = 0;
  /** Events dropped before a reader read them, over every reader, in all. */
  uint64_t missed = 0;
};

namespace detail
{

#if defined(_WIN32)
/** The calling thread's platform id (GetCurrentThreadId()). */
std::uintptr_t currentThreadId();
#else
/**
 * A pthread_t as a number: an integer on Linux, a pointer elsewhere (only the
 * branch that applies is compiled, as this is a template).
 * @param thread the thread
 * @return its id
 */
template <typename TThread>
std::uintptr_t threadIdOf(TThread thread)
{
  if constexpr (std::is_pointer_v<TThread>)
    return reinterpret_cast<std::uintptr_t>(thread);
  else
    return static_cast<std::uintptr_t>(thread);
}

/**
 * The calling thread's platform id (pthread_self()), inline: every send
 * compares it with its owner's.
 */
inline std::uintptr_t currentThreadId() { return threadIdOf(pthread_self()); }
#endif

/**
 * Logs a worker's send, naming the thread (debugFeatures::logEvents()).
 * @param type the event type's name, for people
 */
void logWorkerSend(std::string_view type);

/**
 * Where workers' events of one type wait for the next frame (EventSender).
 * Shared by the queue and its senders, so a sender that outlives the queue
 * finds it closed rather than freed.
 */
template <typename T>
struct EventInbox
{
  /** Guards `staged` and `open`. */
  std::mutex mutex;
  /** The workers' events, in the order they were sent. */
  std::vector<T> staged;
  /**
   * The owning thread's: swapped with `staged` at a frame's start, emptied
   * into the queue, and handed back next time with its room kept.
   */
  std::vector<T> spare;
  /** How many events are staged, read at a frame's start without the lock. */
  std::atomic<uint32_t> count{0};
  /** False once the queue is gone: sends then fail. */
  bool open = true;
  /** Whether each send is logged (debugFeatures::logEvents()). */
  bool log = false;
  /** The event type's name, for the log; valid while `open`. */
  std::string_view name;
};

/**
 * What an event queue does that doesn't depend on its type, so Events can
 * keep the queues of every type in one list.
 */
class EventQueueBase
{
public:
  /** Kept events past which a queue warns: a send loop, or no updates. */
  static constexpr std::size_t kManyEvents = 65536;

  /**
   * @param typeKey the event type's key, componentTypeKey<T>()
   * @param frame the owning Events' frame count, for the log
   */
  EventQueueBase(std::string_view typeKey, const uint64_t& frame);
  virtual ~EventQueueBase() = default;
  EventQueueBase(const EventQueueBase&) = delete;
  EventQueueBase& operator=(const EventQueueBase&) = delete;

  /**
   * The frame boundary: the older frame's events go, this frame's become the
   * older, and the workers' staged events are taken in. Events::update()
   * calls it only while busy().
   */
  virtual void swap() = 0;

  /**
   * This queue's counts, for the debug view.
   * @param out filled with them
   */
  virtual void stats(EventStats& out) const = 0;

  /**
   * Whether swap() has work: events kept, or workers' events staged. Not
   * virtual, so an idle queue costs Events::update() a load or two.
   */
  inline bool busy() const
  {
    return m_kept ||
           (m_staged && m_staged->load(std::memory_order_relaxed) != 0);
  }

  /**
   * Logs a send with its number and frame (debugFeatures::logEvents()).
   * @param id the event's number
   */
  void logSent(uint64_t id) const;
  /**
   * Logs workers' events just taken in.
   * @param first the first one's number
   * @param count how many there were
   */
  void logTaken(uint64_t first, std::size_t count) const;
  /**
   * Warns, once per queue, that many events are kept: a send loop, or a scene
   * nobody updates. Only while debug features are enabled.
   * @param kept how many are kept
   */
  void warnMany(std::size_t kept);
  /**
   * Counts events a reader missed, and warns once for that reader, in every
   * build.
   * @param count how many it missed
   * @param logged the reader's flag: whether it has warned already (set here)
   */
  void missed(uint64_t count, bool& logged);
  /**
   * Logs an error, once per queue, for a send of this type while one of its
   * ranges was being gone through (EventRange's debug check).
   */
  void sentWhileRead();

protected:
  // Whether either buffer holds events: set by a send, cleared by the swap
  // that leaves both empty.
  bool m_kept = false;
  // The inbox's staged count, once there is an inbox.
  const std::atomic<uint32_t>* m_staged = nullptr;
  std::string m_name;
  // Whether each send is logged (debugFeatures::logEvents()).
  bool m_log = false;
  const uint64_t& m_frame;
  uint64_t m_missed = 0;
  bool m_warnedMany = false;
  bool m_warnedSentWhileRead = false;
};

/**
 * The events of one type: this frame's and the last frame's, in two buffers
 * swapped once a frame. Events are numbered in send order from 0, so a reader
 * is just the number of the next event it hasn't read.
 */
template <typename T>
class EventQueue final : public EventQueueBase
{
  // Checked here, where every path (send, reader, sender) makes the queue.
  static_assert(std::is_object_v<T> && !std::is_pointer_v<T> &&
                    !std::is_const_v<T> && !std::is_volatile_v<T>,
                "An event is a value: a struct, not a pointer, reference or "
                "const type");
  static_assert(std::is_nothrow_move_constructible_v<T>,
                "An event type must be nothrow-movable");

public:
  using EventQueueBase::EventQueueBase;

  ~EventQueue() override
  {
    if (!m_inbox)
      return;
    // Staged events go now, while T's code is surely loaded.
    std::vector<T> staged;
    {
      const std::lock_guard lock(m_inbox->mutex);
      m_inbox->open = false;
      staged.swap(m_inbox->staged);
      m_inbox->count.store(0, std::memory_order_relaxed);
    }
  }

  /**
   * This frame's events.
   * @return its buffer
   */
  const std::vector<T>& current() const { return m_buffers[m_current]; }
  /**
   * The last frame's events, which go at the next swap.
   * @return its buffer
   */
  const std::vector<T>& older() const { return m_buffers[m_current ^ 1]; }
  /**
   * The number of the oldest event kept: the older buffer's first.
   * @return it
   */
  uint64_t oldestId() const { return m_oldestId; }
  /**
   * The number the next event sent will get.
   * @return it
   */
  uint64_t endId() const
  {
    return m_oldestId + m_buffers[0].size() + m_buffers[1].size();
  }

  /**
   * The kept events numbered `from` (at least oldestId()) on, as the older
   * buffer's tail and this frame's from where they start: where a number's
   * event lives is worked out here alone.
   * @param from the first event's number
   * @param older set to the older buffer's events from `from`
   * @param current set to this frame's events from `from`
   */
  void kept(uint64_t from,
            std::span<const T>& older,
            std::span<const T>& current) const
  {
    const std::vector<T>& olderBuffer = this->older();
    const std::vector<T>& currentBuffer = this->current();
    const uint64_t currentFirst = m_oldestId + olderBuffer.size();
    current = std::span<const T>(currentBuffer.data(), currentBuffer.size());
    if (from < currentFirst)
      older = std::span<const T>(
          olderBuffer.data() + (from - m_oldestId), currentFirst - from);
    else
    {
      older = {};
      current = current.subspan(static_cast<std::size_t>(from - currentFirst));
    }
  }

  /**
   * Appends an event to this frame's.
   * @param event the event, copied or moved in
   */
  template <typename TEvent>
  void push(TEvent&& event)
  {
    std::vector<T>& into = m_buffers[m_current];
    into.push_back(std::forward<TEvent>(event));
    m_kept = true;
    if (into.size() > kManyEvents && !m_warnedMany) [[unlikely]]
      warnMany(into.size() + older().size());
    if (m_log) [[unlikely]]
      logSent(endId() - 1);
  }

  /**
   * The inbox workers' senders share, made the first time.
   * @return it
   */
  const std::shared_ptr<EventInbox<T>>& inbox()
  {
    if (!m_inbox)
    {
      m_inbox = std::make_shared<EventInbox<T>>();
      m_inbox->log = m_log;
      m_inbox->name = m_name;
      m_staged = &m_inbox->count;
    }
    return m_inbox;
  }

  void swap() override
  {
    std::vector<T>& older = m_buffers[m_current ^ 1];
    const std::vector<T>& current = m_buffers[m_current];
    if (older.size() + current.size() > kManyEvents && !m_warnedMany)
      warnMany(older.size() + current.size());
    m_oldestId += older.size();
    older.clear();
    m_current ^= 1;
    if (m_staged && m_staged->load(std::memory_order_relaxed) != 0)
      takeStaged();
    m_kept = !m_buffers[0].empty() || !m_buffers[1].empty();
  }

  void stats(EventStats& out) const override
  {
    out.type = m_name;
    out.sentLastFrame = older().size();
    out.kept = older().size() + current().size();
    out.staged = m_inbox ? m_inbox->count.load(std::memory_order_relaxed) : 0u;
    out.capacity = m_buffers[0].capacity() + m_buffers[1].capacity();
    out.missed = m_missed;
  }

private:
  // Moves the workers' events into this frame's, ahead of any sent on the
  // main thread this frame. The lock covers only the swap of two vectors.
  void takeStaged()
  {
    std::vector<T>& spare = m_inbox->spare;
    {
      const std::lock_guard lock(m_inbox->mutex);
      spare.swap(m_inbox->staged);
      m_inbox->count.store(0, std::memory_order_relaxed);
    }
    std::vector<T>& into = m_buffers[m_current];
    const uint64_t first = endId();
    // insert() copies trivial events as one block, but needs T to be
    // move-assignable; other types go one by one.
    if constexpr (std::is_move_assignable_v<T>)
      into.insert(into.end(),
                  std::make_move_iterator(spare.begin()),
                  std::make_move_iterator(spare.end()));
    else
    {
      into.reserve(into.size() + spare.size());
      for (T& event : spare)
        into.emplace_back(std::move(event));
    }
    if (m_log)
      logTaken(first, spare.size());
    spare.clear();
  }

  std::vector<T> m_buffers[2];
  uint64_t m_oldestId = 0;
  uint8_t m_current = 0;
  std::shared_ptr<EventInbox<T>> m_inbox;
};

} // namespace detail

} // namespace sfs
