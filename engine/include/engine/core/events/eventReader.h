#pragma once

#include "engine/core/events/eventQueue.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>

namespace sfs
{

class Events;

/**
 * The events an EventReader hadn't read, oldest first: the unread tail of the
 * last frame's, then this frame's, gone through as one range of `const T&`.
 * It points into the queue: nothing is copied. Use it at once, in the loop
 * that asked for it, and don't send a T while going through it (the queue may
 * move): collect what to send, then send it. Builds without NDEBUG check this
 * on each step, log an error (once per type) and end the loop rather than read
 * a moved buffer; release builds don't check.
 */
template <typename T>
class EventRange
{
public:
  /** Goes through the two spans in turn. */
  class iterator
  {
  public:
    using iterator_concept = std::forward_iterator_tag;
    using iterator_category = std::forward_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = const T*;
    using reference = const T&;

    iterator() = default;

    const T& operator*() const { return *m_at; }
    const T* operator->() const { return m_at; }

    iterator& operator++()
    {
      if (++m_at == m_end)
      {
        m_at = m_next;
        m_end = m_nextEnd;
        m_next = m_nextEnd;
      }
#ifndef NDEBUG
      if (m_queue && m_queue->endId() != m_sent)
      {
        m_queue->sentWhileRead();
        m_at = m_end = m_next = m_nextEnd;
        m_queue = nullptr;
      }
#endif
      return *this;
    }
    iterator operator++(int)
    {
      iterator before = *this;
      ++*this;
      return before;
    }

    bool operator==(const iterator& other) const { return m_at == other.m_at; }
    /** Whether it has gone through both spans. */
    bool operator==(std::default_sentinel_t) const { return m_at == m_end; }

  private:
    friend class EventRange;

    const T* m_at = nullptr;
    const T* m_end = nullptr;
    const T* m_next = nullptr;
    const T* m_nextEnd = nullptr;
#ifndef NDEBUG
    detail::EventQueue<T>* m_queue = nullptr;
    uint64_t m_sent = 0;
#endif
  };

  /** Empty. */
  EventRange() = default;

  /**
   * @param older the last frame's events still to read
   * @param current this frame's events still to read
   * @param queue the queue they're in, for the check that nothing is sent
   */
  EventRange(std::span<const T> older,
             std::span<const T> current,
             [[maybe_unused]] detail::EventQueue<T>* queue)
      : m_older(older), m_current(current)
#ifndef NDEBUG
        ,
        m_queue(queue), m_sent(queue ? queue->endId() : 0)
#endif
  {
  }

  /** The first event, oldest first. */
  iterator begin() const
  {
    iterator it;
    const std::span<const T>& first = m_older.empty() ? m_current : m_older;
    const std::span<const T>& then = m_older.empty() ? m_older : m_current;
    it.m_at = first.data();
    it.m_end = first.data() + first.size();
    it.m_next = then.empty() ? it.m_end : then.data();
    it.m_nextEnd = then.empty() ? it.m_end : then.data() + then.size();
#ifndef NDEBUG
    it.m_queue = m_queue;
    it.m_sent = m_sent;
#endif
    return it;
  }
  /** Past the last event. */
  std::default_sentinel_t end() const { return {}; }

  /** How many events it holds. */
  std::size_t size() const { return m_older.size() + m_current.size(); }
  /** Whether it holds none. */
  bool empty() const { return size() == 0; }

private:
  std::span<const T> m_older;
  std::span<const T> m_current;
#ifndef NDEBUG
  detail::EventQueue<T>* m_queue = nullptr;
  uint64_t m_sent = 0;
#endif
};

/**
 * A place in one event type's queue: read() gives the events sent since the
 * last read(), oldest first, so each reader sees each event once. Make one
 * with Events::reader<T>() in a system's create() (or an object's onCreate)
 * and keep it as a member: a reader made anew each update reads the kept
 * events again. 24 bytes; it allocates nothing. One made by default reads
 * nothing. Only the Events' owning thread reads.
 *
 * Events are kept until the second frame boundary after they were sent: a
 * reader that doesn't read for that long misses some, and its next read()
 * starts at the oldest kept. A miss is logged as a warning once per reader, in
 * every build, and counted in the debug view: read every reader each update,
 * before any early return.
 */
template <typename T>
class EventReader
{
public:
  /** A reader of nothing: read() is always empty. */
  EventReader() = default;

  /**
   * The events this reader hasn't read, oldest first, all marked read (a loop
   * that breaks early skips the rest).
   * @return a range over them, valid until the next send or swap
   */
  EventRange<T> read()
  {
    if (!m_queue)
      return {};
    detail::EventQueue<T>& queue = *m_queue;
    const uint64_t oldest = queue.oldestId();
    if (m_next < oldest) [[unlikely]]
    {
      queue.missed(oldest - m_next, m_loggedMissed);
      m_next = oldest;
    }
    std::span<const T> olderTail;
    std::span<const T> currentTail;
    queue.kept(m_next, olderTail, currentTail);
    m_next = queue.endId();
    return EventRange<T>(olderTail, currentTail, &queue);
  }

  /**
   * How many events read() would give now, without marking them read.
   * @return the count
   */
  std::size_t unread() const
  {
    if (!m_queue)
      return 0;
    const uint64_t from =
        m_next < m_queue->oldestId() ? m_queue->oldestId() : m_next;
    return static_cast<std::size_t>(m_queue->endId() - from);
  }

  /** Marks every kept event read, as a read() whose range is unused would. */
  void skip()
  {
    if (m_queue)
      m_next = m_queue->endId();
  }

private:
  friend class Events;

  // A reader at the oldest event `queue` keeps.
  explicit EventReader(detail::EventQueue<T>& queue)
      : m_queue(&queue), m_next(queue.oldestId())
  {
  }

  detail::EventQueue<T>* m_queue = nullptr;
  uint64_t m_next = 0;
  bool m_loggedMissed = false;
};

} // namespace sfs
