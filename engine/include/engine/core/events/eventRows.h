#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/events/eventQueue.h"
#include "engine/core/events/eventRules.h"
#include "engine/core/events/keyed.h"
#include "engine/core/events/requestTickets.h"
#include "engine/core/events/responder.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <type_traits>
#include <utility>

// The rows a consumer gives (eventConsumer.h): what a read or a handler goes
// through.
namespace sfs
{

/**
 * A keyed event's row: the entity it's about and the event, unpacked as
 * `auto [entity, event]`. It converts to the event alone, for a loop that
 * doesn't need the entity.
 */
template <typename TEvent>
struct KeyedRow
{
  /** The entity the event is about. */
  const Entity& entity;
  /** The event. */
  const TEvent& event;

  /** The event alone. */
  operator const TEvent&() const { return event; }
};

/**
 * A request's row: the entity it's about, the request and the Responder that
 * responds to it, unpacked as `auto [entity, request, responder]`.
 */
template <typename TRequest>
struct RequestRow
{
  /** The entity the request is about. */
  const Entity& entity;
  /** The request. */
  const TRequest& request;
  /** How to respond to it, from any thread. */
  Responder<TRequest> responder;
};

namespace detail
{

/** Every event of the span: a read. */
struct PickAll
{
  /** Whether to give event `i` of a segment: always. */
  template <typename TEvent>
  bool accept(const EventSegment<TEvent>&, std::size_t) const
  {
    return true;
  }
};

/**
 * Events still current for their key: readLatest(). A request is current
 * while its ticket holds it: its requester's newest about its key.
 */
struct PickCurrent
{
  /** The keyed type's channel. */
  const KeyedChannel* channel = nullptr;

  /** Whether event `i` of a segment is still its key's current one. */
  template <typename TEvent>
  bool accept(const EventSegment<TEvent>& part, std::size_t i) const
  {
    if (part.tickets)
      return channel->tickets->current(part.tickets[i], part.stamps[i]);
    return channel->current(part.keys[i].getId(), part.stamps[i]);
  }
};

/** What a range of TEvent gives by default: every event. */
template <typename TEvent>
using DefaultPick = PickAll;

} // namespace detail

/**
 * Events a consumer gives at once, oldest first: a read()'s or a batch
 * handler's. Each row is `const E&` for an unkeyed type, `[entity, event]`
 * for a keyed one, and `[entity, request, responder]` for a request. A
 * requester's (ResponseRange) give only responses to each key's current
 * request. It points into the queue: nothing is copied. While it lives, its
 * queue doesn't move them: an E sent meanwhile (by a handler, say, or as a
 * derived event's copy) waits and is numbered after them, for the next span.
 * Use it in the loop that asked for it: a range kept across a frame boundary
 * holds its queue as it is until it goes.
 */
template <typename TEvent,
          typename TPick = detail::DefaultPick<TEvent>,
          bool kResponders = detail::kRequest<TEvent>>
class EventRange
{
  using Segment = detail::EventSegment<TEvent>;

public:
  /**
   * Goes through the events given: a std::forward_iterator, so the range
   * works with std::ranges algorithms. A keyed type's rows are made as they're
   * read (proxies, as a transform view's are), so to the older algorithms
   * it's an input iterator.
   */
  class iterator
  {
  public:
    /** A row as it's read: `const E&`, a KeyedRow or a RequestRow. */
    using reference =
        std::conditional_t<kResponders,
                           RequestRow<TEvent>,
                           std::conditional_t<detail::kKeyed<TEvent>,
                                              KeyedRow<TEvent>,
                                              const TEvent&>>;
    /** A row's type. */
    using value_type = std::remove_cvref_t<reference>;
    using iterator_concept = std::forward_iterator_tag;
    using iterator_category = std::conditional_t<std::is_reference_v<reference>,
                                                 std::forward_iterator_tag,
                                                 std::input_iterator_tag>;
    using difference_type = std::ptrdiff_t;

    iterator() = default;

    /** The row: `const E&`, a KeyedRow or a RequestRow. */
    reference operator*() const { return m_range->row(m_at); }
    /** The event of an unkeyed type's row. */
    const TEvent* operator->() const
      requires(!detail::kKeyed<TEvent>)
    {
      return m_range->m_segment.events + m_at;
    }

    iterator& operator++()
    {
      ++m_at;
      settle();
      return *this;
    }
    iterator operator++(int)
    {
      iterator before = *this;
      ++*this;
      return before;
    }

    bool operator==(const iterator& other) const { return m_at == other.m_at; }
    /** Whether it has gone through every event. */
    bool operator==(std::default_sentinel_t) const
    {
      return m_at >= m_range->m_segment.count;
    }

  private:
    friend class EventRange;

    // Moves on to the next event given, or past the end.
    void settle()
    {
      const Segment& part = m_range->m_segment;
      while (m_at < part.count && !m_range->m_pick.accept(part, m_at))
        ++m_at;
    }

    const EventRange* m_range = nullptr;
    std::size_t m_at = 0;
  };

  /** No events. */
  EventRange() = default;

  /**
   * @param segment the span's events
   * @param pick which events to give
   * @param queue the queue they're in, which keeps them where they are
   *        while the range lives
   * @param channel the request type's channel, for responders (kResponders)
   */
  EventRange(const Segment& segment,
             const TPick& pick,
             detail::EventQueueBase* queue,
             detail::KeyedChannel* channel = nullptr)
      : m_segment(segment), m_pick(pick), m_channel(channel), m_queue(queue)
  {
    if (m_queue)
      m_queue->beginRead();
  }

  /** The same events, keeping them where they are while it lives too. */
  EventRange(const EventRange& other)
      : m_segment(other.m_segment), m_pick(other.m_pick),
        m_channel(other.m_channel), m_queue(other.m_queue)
  {
    if (m_queue)
      m_queue->beginRead();
  }

  /** Takes over `other`'s events, leaving it empty. */
  EventRange(EventRange&& other) noexcept
      : m_segment(other.m_segment), m_pick(other.m_pick),
        m_channel(other.m_channel), m_queue(other.m_queue)
  {
    other.m_segment = {};
    other.m_queue = nullptr;
  }

  /** As the copy constructor, letting go of its own events first. */
  EventRange& operator=(const EventRange& other)
  {
    if (this != &other)
    {
      EventRange copy(other);
      *this = std::move(copy);
    }
    return *this;
  }

  /** As the move constructor, letting go of its own events first. */
  EventRange& operator=(EventRange&& other) noexcept
  {
    if (this != &other)
    {
      if (m_queue)
        m_queue->endRead();
      m_segment = other.m_segment;
      m_pick = other.m_pick;
      m_channel = other.m_channel;
      m_queue = other.m_queue;
      other.m_segment = {};
      other.m_queue = nullptr;
    }
    return *this;
  }

  /** Lets the queue move its events again once no range is left. */
  ~EventRange()
  {
    if (m_queue)
      m_queue->endRead();
  }

  /** The first event. */
  iterator begin() const
  {
    iterator it;
    it.m_range = this;
    it.settle();
    return it;
  }
  /** Past the last event. */
  std::default_sentinel_t end() const { return {}; }

  /** Whether it gives no events. */
  bool empty() const { return begin() == end(); }

  /** How many events it gives (counted when some may be passed over). */
  std::size_t size() const
  {
    if constexpr (std::is_same_v<TPick, detail::PickAll>)
      return m_segment.count;
    else
    {
      std::size_t count = 0;
      for (iterator it = begin(); it != end(); ++it)
        ++count;
      return count;
    }
  }

private:
  friend class iterator;

  decltype(auto) row(std::size_t at) const
  {
    if constexpr (kResponders)
      return RequestRow<TEvent>{m_segment.keys[at],
                                m_segment.events[at],
                                Responder<TEvent>(*m_channel,
                                                  m_segment.keys[at],
                                                  m_segment.stamps[at],
                                                  m_segment.tickets[at])};
    else if constexpr (detail::kKeyed<TEvent>)
      return KeyedRow<TEvent>{m_segment.keys[at], m_segment.events[at]};
    else
      return (m_segment.events[at]);
  }

  Segment m_segment;
  TPick m_pick;
  detail::KeyedChannel* m_channel = nullptr;
  detail::EventQueueBase* m_queue = nullptr;
};

/**
 * The rows of readLatest() and a latest handler: each key's newest current
 * event, with its Responder for a request.
 */
template <typename TEvent>
using LatestEventRange =
    EventRange<TEvent, detail::PickCurrent, detail::kRequest<TEvent>>;

/**
 * The responses of type A to requests of type R that one requester's
 * handlers get (onResponses(), onProgresses(), onFailures()): rows
 * `[key, response]`, only those to its own current requests, oldest first.
 * Each response is routed to its requester as it's kept, so a requester goes
 * through its own alone, however many others there are. Like an EventRange, it
 * keeps its queue's events where they are while it lives: use it in the handler
 * that gets it.
 */
template <typename TRequest, typename TResponse>
class ResponseRange
{
  using T = detail::ResponseTo<TRequest, TResponse>;
  using Segment = detail::EventSegment<T>;

public:
  /**
   * Goes through the requester's responses: a std::forward_iterator whose
   * rows are made as they're read (an input iterator to the older
   * algorithms).
   */
  class iterator
  {
  public:
    /** A row as it's read: the request's key and the response. */
    using reference = KeyedRow<T>;
    /** A row's type. */
    using value_type = KeyedRow<T>;
    using iterator_concept = std::forward_iterator_tag;
    using iterator_category = std::input_iterator_tag;
    using difference_type = std::ptrdiff_t;

    iterator() = default;

    /** The row: the request's key and the response. */
    KeyedRow<T> operator*() const
    {
      const std::size_t at = m_range->indexOf(m_at);
      return KeyedRow<T>{
          m_range->m_segment.keys[at], m_range->m_segment.events[at]};
    }

    iterator& operator++()
    {
      ++m_at;
      settle();
      return *this;
    }
    iterator operator++(int)
    {
      iterator before = *this;
      ++*this;
      return before;
    }

    bool operator==(const iterator& other) const { return m_at == other.m_at; }
    /** Whether it has gone through every response. */
    bool operator==(std::default_sentinel_t) const
    {
      return m_at >= m_range->m_count;
    }

  private:
    friend class ResponseRange;

    // Moves on to the next response still current, or past the end.
    void settle()
    {
      while (m_at < m_range->m_count && !m_range->accept(m_at))
        ++m_at;
    }

    const ResponseRange* m_range = nullptr;
    std::size_t m_at = 0;
  };

  /** No responses. */
  ResponseRange() = default;

  /**
   * @param segment the requester's span's events
   * @param first the span's first event's number
   * @param ids the numbers of the requester's responses in it
   * @param count how many
   * @param channel the request type's channel
   * @param requester the requester's number
   * @param queue the queue, kept still while the range lives
   */
  ResponseRange(const Segment& segment,
                uint64_t first,
                const uint64_t* ids,
                std::size_t count,
                const detail::KeyedChannel* channel,
                uint32_t requester,
                detail::EventQueueBase* queue)
      : m_segment(segment), m_first(first), m_ids(ids), m_count(count),
        m_channel(channel), m_requester(requester), m_queue(queue)
  {
    if (m_queue)
      m_queue->beginRead();
  }

  /** The same responses, keeping them where they are while it lives too. */
  ResponseRange(const ResponseRange& other)
      : m_segment(other.m_segment), m_first(other.m_first), m_ids(other.m_ids),
        m_count(other.m_count), m_channel(other.m_channel),
        m_requester(other.m_requester), m_queue(other.m_queue)
  {
    if (m_queue)
      m_queue->beginRead();
  }

  ResponseRange& operator=(const ResponseRange&) = delete;

  /** Lets the queue move its events again once no range is left. */
  ~ResponseRange()
  {
    if (m_queue)
      m_queue->endRead();
  }

  /** The first response. */
  iterator begin() const
  {
    iterator it;
    it.m_range = this;
    it.settle();
    return it;
  }
  /** Past the last response. */
  std::default_sentinel_t end() const { return {}; }

  /** Whether it gives no responses. */
  bool empty() const { return begin() == end(); }

  /** How many responses it gives. */
  std::size_t size() const
  {
    std::size_t count = 0;
    for (iterator it = begin(); it != end(); ++it)
      ++count;
    return count;
  }

private:
  friend class iterator;

  std::size_t indexOf(std::size_t at) const
  {
    return static_cast<std::size_t>(m_ids[at] - m_first);
  }

  // Whether the at-th routed response is in the span and answers the
  // requester's current request.
  bool accept(std::size_t at) const
  {
    if (m_ids[at] < m_first || m_ids[at] - m_first >= m_segment.count)
      return false;
    const std::size_t index = indexOf(at);
    const detail::RequestTickets& requests = *m_channel->tickets;
    return requests.current(
               m_segment.tickets[index], m_segment.stamps[index]) &&
           requests.requesterOf(m_segment.tickets[index]) == m_requester;
  }

  Segment m_segment;
  uint64_t m_first = 0;
  const uint64_t* m_ids = nullptr;
  std::size_t m_count = 0;
  const detail::KeyedChannel* m_channel = nullptr;
  uint32_t m_requester = 0;
  detail::EventQueueBase* m_queue = nullptr;
};

} // namespace sfs
