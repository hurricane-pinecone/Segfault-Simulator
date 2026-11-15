#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/events/eventOwner.h"
#include "engine/core/events/eventQueue.h"
#include "engine/core/events/eventRows.h"
#include "engine/core/events/eventRules.h"
#include "engine/core/events/events.h"
#include "engine/core/events/keyed.h"
#include "engine/core/util/typeKey.h"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

// The handles that receive events: consumers and requesters
// (docs/core/events).
namespace sfs
{

namespace detail
{

/**
 * Logs, once per call site's type, handlers given to a handle with no owner:
 * one made outside create() or onCreate, whose handlers never run.
 * @param type the event type's name
 */
void handlersWithoutOwner(std::string_view type);

// The rows a consumer's span gives, for every event or each key's current
// one.
template <typename TEvent, bool kLatest>
auto spanRange(EventQueue<TEvent>& queue, uint32_t slot)
{
  uint64_t begin = 0;
  uint64_t end = 0;
  queue.span(slot, kLatest ? Consuming::Latest : Consuming::All, begin, end);
  const EventSegment<TEvent> part = queue.segment(begin, end);
  if constexpr (kLatest)
    return LatestEventRange<TEvent>(part,
                                    PickCurrent{queue.channel().get()},
                                    &queue,
                                    queue.channel().get());
  else if constexpr (kKeyed<TEvent>)
    return EventRange<TEvent>(part, PickAll{}, &queue, queue.channel().get());
  else
    return EventRange<TEvent>(part, PickAll{}, &queue);
}

// The responses a requester's span gives: those to its own current
// requests.
template <typename TRequest, typename TResponse>
ResponseRange<TRequest, TResponse>
responseRange(EventQueue<ResponseTo<TRequest, TResponse>>& queue,
              uint32_t slot,
              uint32_t requester)
{
  uint64_t begin = 0;
  uint64_t end = 0;
  queue.span(slot, Consuming::All, begin, end);
  const std::vector<uint64_t>& ids = queue.takeRoute(requester, end);
  return ResponseRange<TRequest, TResponse>(queue.segment(begin, end),
                                            std::max(begin, queue.oldestId()),
                                            ids.data(),
                                            ids.size(),
                                            queue.channel().get(),
                                            requester,
                                            &queue);
}

// Calls a per-event handler with a row, unpacked: (event), (entity, event)
// or (entity, request, responder).
template <typename TEvent, typename TFunction, typename TRow>
void callWithRow(TFunction& function, TRow&& row)
{
  if constexpr (kRequest<TEvent>)
    function(row.entity, row.request, row.responder);
  else if constexpr (kKeyed<TEvent>)
    function(row.entity, row.event);
  else
    function(row);
}

// Whether a per-event handler takes a TEvent row, unpacked.
template <typename TEvent, typename TFunction>
concept RowHandler =
    (kRequest<TEvent> && std::invocable<TFunction&,
                                        const Entity&,
                                        const TEvent&,
                                        Responder<TEvent>&>) ||
    (!kRequest<TEvent> && kKeyed<TEvent> &&
     std::invocable<TFunction&, const Entity&, const TEvent&>) ||
    (!kKeyed<TEvent> && std::invocable<TFunction&, const TEvent&>);

} // namespace detail

/**
 * A consumer of one event type: a place in its queue, which keeps every
 * event until each of its consumers has passed it. A system makes one in
 * create() with consumer<E>() and keeps it as a member; an object makes one
 * in onCreate with scene().events().consumer<E>(). Every consumer sees every
 * event once: nothing is consumed or removed for the others.
 *
 * Read the events in update() (read(), readLatest()), or give the consumer
 * handlers once, as it's made (onEvent(), onEvents(), onLatest(),
 * onLatestEvents()): the engine calls them in its owner's slot, before a
 * system's update() or an object's onUpdate(), and not at all when nothing
 * is new. A read in an update gives the same span its handlers got: what
 * arrived since the last update. A consumer with no owner (made elsewhere)
 * runs no handlers, and each read gives what arrived since the last.
 *
 * It's registered when it's made, so nothing sent from then on is lost
 * however late it's first read; one never read holds the queue (warned at
 * 65,536 kept, dropped at the cap and counted as missed). It deregisters as
 * it's destroyed, so it is moved, never copied; one outliving its events
 * reads nothing. Only the events' owning thread reads.
 */
template <typename TEvent>
class EventConsumer
{
  static_assert(detail::checkEvent<TEvent>());

public:
  /** A consumer of nothing. */
  EventConsumer() = default;
  EventConsumer(const EventConsumer&) = delete;
  EventConsumer& operator=(const EventConsumer&) = delete;

  /** Takes over `other`'s place, leaving it a consumer of nothing. */
  EventConsumer(EventConsumer&& other) noexcept
      : m_link(std::move(other.m_link)), m_owner(other.m_owner),
        m_slot(other.m_slot)
  {
    other.m_owner = nullptr;
  }

  /** Lets go of its own place, then takes over `other`'s. */
  EventConsumer& operator=(EventConsumer&& other) noexcept
  {
    if (this != &other)
    {
      release();
      m_link = std::move(other.m_link);
      m_owner = other.m_owner;
      m_slot = other.m_slot;
      other.m_owner = nullptr;
    }
    return *this;
  }

  /** Deregisters: the queue no longer keeps events for it. */
  ~EventConsumer() { release(); }

  /**
   * The events that arrived since the last span, oldest first. A request's
   * rows carry its Responder.
   * @return the rows: `const E&`, `[entity, event]` for a keyed type,
   *         `[entity, request, responder]` for a request
   */
  EventRange<TEvent> read()
  {
    detail::EventQueue<TEvent>* queue = this->queue();
    if (!queue)
      return {};
    return detail::spanRange<TEvent, false>(*queue, m_slot);
  }

  /**
   * Each key's newest current event that arrived since the last span (not
   * superseded by a newer send, a cancel or its entity's destruction).
   * @return the rows, `[entity, event]` (`[entity, request, responder]` for a
   *         request)
   */
  LatestEventRange<TEvent> readLatest()
  {
    static_assert(detail::kOwnChannel<TEvent>,
                  "readLatest() reads [[sfs::event_entity_news]] or a "
                  "request");
    detail::EventQueue<TEvent>* queue = this->queue();
    if (!queue)
      return {};
    return detail::spanRange<TEvent, true>(*queue, m_slot);
  }

  /**
   * Gives it a handler for each new event, called in its owner's slot:
   * `fn(const E&)`, `fn(const Entity&, const E&)` for a keyed type, or
   * `fn(const Entity&, const R&, const Responder<R>&)` for a request (by
   * value it takes a hold on the responder's block). Chain it as the
   * consumer is made: `m_kills = consumer<EnemyDiedEvent>().onEvent(...);`.
   * @param fn the handler
   * @return this consumer, for more
   */
  template <typename TFunction>
  EventConsumer& onEvent(TFunction fn) &
  {
    addEventHandler(std::move(fn));
    return *this;
  }
  /** As onEvent() on a consumer just made, to be assigned. */
  template <typename TFunction>
  EventConsumer&& onEvent(TFunction fn) &&
  {
    addEventHandler(std::move(fn));
    return std::move(*this);
  }

  /**
   * Gives it a handler for each update's new events at once, called in its
   * owner's slot when there are some: `fn(sfs::EventRange<E>)`.
   * @param fn the handler
   * @return this consumer, for more
   */
  template <typename TFunction>
  EventConsumer& onEvents(TFunction fn) &
  {
    addEventsHandler(std::move(fn));
    return *this;
  }
  /** As onEvents() on a consumer just made, to be assigned. */
  template <typename TFunction>
  EventConsumer&& onEvents(TFunction fn) &&
  {
    addEventsHandler(std::move(fn));
    return std::move(*this);
  }

  /**
   * Gives it a handler for each key's newest current event among the new
   * ones (keyed types), called in its owner's slot, unpacked as onEvent()'s.
   * @param fn the handler
   * @return this consumer, for more
   */
  template <typename TFunction>
  EventConsumer& onLatest(TFunction fn) &
  {
    addLatestHandler(std::move(fn));
    return *this;
  }
  /** As onLatest() on a consumer just made, to be assigned. */
  template <typename TFunction>
  EventConsumer&& onLatest(TFunction fn) &&
  {
    addLatestHandler(std::move(fn));
    return std::move(*this);
  }

  /**
   * Gives it a handler for each update's newest current events per key at
   * once (keyed types), called in its owner's slot when there are some:
   * `fn(sfs::LatestEventRange<E>)`.
   * @param fn the handler
   * @return this consumer, for more
   */
  template <typename TFunction>
  EventConsumer& onLatestEvents(TFunction fn) &
  {
    addLatestEventsHandler(std::move(fn));
    return *this;
  }
  /** As onLatestEvents() on a consumer just made, to be assigned. */
  template <typename TFunction>
  EventConsumer&& onLatestEvents(TFunction fn) &&
  {
    addLatestEventsHandler(std::move(fn));
    return std::move(*this);
  }

  /**
   * How many events it hasn't read yet, past its place (those its next span
   * will give).
   * @return the count
   */
  std::size_t unread() const
  {
    const detail::EventQueueBase* queue = queueBase();
    if (!queue)
      return 0;
    const uint64_t next = queue->consumer(m_slot).next;
    const uint64_t from = next < queue->oldestId() ? queue->oldestId() : next;
    return static_cast<std::size_t>(queue->endId() - from);
  }

  /** Passes every kept event without going through them, this update's too. */
  void skip()
  {
    if (detail::EventQueueBase* queue = queueBase())
    {
      detail::ConsumerSlot& self = queue->consumer(m_slot);
      self.next = queue->endId();
      // An update's span already fixed is passed too.
      self.spanBegin = self.spanEnd;
    }
  }

  /**
   * Whether it's registered with a queue that lives (false for one made by
   * default, refused, moved from, or outliving its events).
   * @return whether it reads
   */
  bool valid() const { return queueBase() != nullptr; }

private:
  friend class Events;
  friend class EventVerbs;

  // Registers a consumer with `queue`, at the oldest event kept, owned by
  // `owner` (null: none).
  EventConsumer(detail::EventQueue<TEvent>& queue,
                detail::EventOwner* owner,
                const std::type_info* who)
      : m_link(queue.link()), m_owner(owner),
        m_slot(queue.addConsumer(owner, who))
  {
    if (m_owner)
      m_owner->add(m_link, m_slot);
  }

  // The handlers' bodies, run by the owner over the consumer's span.
  template <typename TFunction>
  void addEventHandler(TFunction fn)
  {
    static_assert(detail::RowHandler<TEvent, TFunction>,
                  "onEvent() takes fn(const E&), fn(const Entity&, const E&) "
                  "for a keyed type, or fn(const Entity&, const R&, "
                  "const Responder<R>&) for a request");
    addHandler(
        [fn = std::move(fn)](
            detail::EventQueueBase& base, uint32_t slot) mutable
        {
          auto& queue = static_cast<detail::EventQueue<TEvent>&>(base);
          for (auto&& row : detail::spanRange<TEvent, false>(queue, slot))
            detail::callWithRow<TEvent>(fn, row);
        });
  }

  template <typename TFunction>
  void addEventsHandler(TFunction fn)
  {
    static_assert(std::invocable<TFunction&, EventRange<TEvent>>,
                  "onEvents() takes fn(sfs::EventRange<E>)");
    addHandler(
        [fn = std::move(fn)](
            detail::EventQueueBase& base, uint32_t slot) mutable
        {
          auto& queue = static_cast<detail::EventQueue<TEvent>&>(base);
          EventRange<TEvent> events =
              detail::spanRange<TEvent, false>(queue, slot);
          if (!events.empty())
            fn(events);
        });
  }

  template <typename TFunction>
  void addLatestHandler(TFunction fn)
  {
    static_assert(detail::kOwnChannel<TEvent>,
                  "onLatest() takes [[sfs::event_entity_news]] or a "
                  "request");
    static_assert(detail::RowHandler<TEvent, TFunction>,
                  "onLatest() takes fn(const Entity&, const E&), or "
                  "fn(const Entity&, const R&, const Responder<R>&) for a "
                  "request");
    addHandler(
        [fn = std::move(fn)](
            detail::EventQueueBase& base, uint32_t slot) mutable
        {
          auto& queue = static_cast<detail::EventQueue<TEvent>&>(base);
          for (auto&& row : detail::spanRange<TEvent, true>(queue, slot))
            detail::callWithRow<TEvent>(fn, row);
        });
  }

  template <typename TFunction>
  void addLatestEventsHandler(TFunction fn)
  {
    static_assert(detail::kOwnChannel<TEvent>,
                  "onLatestEvents() takes an [[sfs::event_entity_news]] or a "
                  "request");
    static_assert(std::invocable<TFunction&, LatestEventRange<TEvent>>,
                  "onLatestEvents() takes fn(sfs::LatestEventRange<E>)");
    addHandler(
        [fn = std::move(fn)](
            detail::EventQueueBase& base, uint32_t slot) mutable
        {
          auto& queue = static_cast<detail::EventQueue<TEvent>&>(base);
          LatestEventRange<TEvent> events =
              detail::spanRange<TEvent, true>(queue, slot);
          if (!events.empty())
            fn(events);
        });
  }

  detail::EventQueueBase* queueBase() const
  {
    return m_link ? m_link->queue : nullptr;
  }
  detail::EventQueue<TEvent>* queue() const
  {
    return static_cast<detail::EventQueue<TEvent>*>(queueBase());
  }

  // Hands a handler to the owner, which runs it in its slot.
  void addHandler(std::function<void(detail::EventQueueBase&, uint32_t)> run)
  {
    if (!m_link)
      return;
    if (!m_owner) [[unlikely]]
    {
      if (const detail::EventQueueBase* queue = queueBase())
        detail::handlersWithoutOwner(queue->name());
      return;
    }
    m_owner->addHandler({m_link, m_slot, std::move(run)});
  }

  // Frees its slot and its handlers. The owner is the slot's, which its
  // owner clears as it goes, so a consumer outliving its owner is safe.
  void release()
  {
    if (!m_link)
      return;
    if (detail::EventQueueBase* queue = m_link->queue)
    {
      if (detail::EventOwner* owner = queue->consumer(m_slot).owner)
        owner->forget(m_link.get(), m_slot);
      queue->removeConsumer(m_slot);
    }
    m_link.reset();
    m_owner = nullptr;
  }

  std::shared_ptr<detail::EventQueueLink> m_link;
  detail::EventOwner* m_owner = nullptr;
  uint32_t m_slot = 0;
};

namespace detail
{

/**
 * Fails to compile, naming the request and the response, when a requester
 * becomes an EventRequester<R> with one of R's responses neither handled nor
 * ignored.
 */
template <typename TRequest, typename TResponse>
struct ResponseHasNoHandler
{
  static_assert(sizeof(TResponse) == 0,
                "A request's response (ResponseHasNoHandler<Request, "
                "Response>) has no onResponse handler; handle it with "
                "onResponse<Response>() "
                "or onResponses<Response>(), or .ignore<Response>()");
};

/**
 * Fails to compile, naming the request and the type, when a requester is
 * given a handler for a type its request doesn't list.
 */
template <typename TRequest, typename TResponse>
struct ResponseNotInRequest
{
  static_assert(sizeof(TResponse) == 0,
                "onResponse<A>(), onResponses<A>() and ignore<A>() take one of "
                "the request's responses; this type (ResponseNotInRequest<"
                "Request, Response>) isn't listed");
};

/**
 * Fails to compile, naming the request and the response, when a requester
 * handles or ignores a response twice.
 */
template <typename TRequest, typename TResponse>
struct ResponseHandledTwice
{
  static_assert(sizeof(TResponse) == 0,
                "A requester handles or ignores each response once; this one "
                "(ResponseHandledTwice<Request, Response>) is twice");
};

// A requester builder's step: A is one of R's responses, not handled yet.
template <typename TRequest, typename TResponse, typename... THandled>
constexpr void checkResponseStep()
{
  if constexpr (!kListed<TResponse, typename EventTraits<TRequest>::Responses>)
    static_cast<void>(sizeof(ResponseNotInRequest<TRequest, TResponse>));
  else if constexpr (kCount<TResponse, THandled...> != 0)
    static_cast<void>(sizeof(ResponseHandledTwice<TRequest, TResponse>));
}

// One of R's responses is handled or ignored.
template <typename TRequest, typename TResponse, typename... THandled>
constexpr void checkResponseHandled()
{
  if constexpr (kCount<TResponse, THandled...> == 0)
    static_cast<void>(sizeof(ResponseHasNoHandler<TRequest, TResponse>));
}

// Every one of R's responses is handled or ignored.
template <typename TRequest, typename... THandled>
struct AllResponsesHandled
{
  template <typename... TResponses>
  static constexpr void check(TypeList<TResponses...>*)
  {
    (checkResponseHandled<TRequest, TResponses, THandled...>(), ...);
  }
};

} // namespace detail

/**
 * Asks for a request type R ([[sfs::event_request(...)]]) and receives its
 * responses: a system makes one in create() and keeps it as a member (an
 * object in onCreate, through scene().events()), built in one chain that
 * handles each of R's response types or ignores it:
 *
 *   m_paths = requester<PathRequest>()
 *                 .onResponse<PathFound>(...)
 *                 .ignore<PathFailed>();
 *
 * then asks with request(). The handlers run in its owner's slot, before a
 * system's update() or an object's onUpdate(), and see only responses to each
 * key's current request: a newer request, a cancel or the key entity's
 * destruction drops the older one's responses. Responses are events too, so
 * others may consumer<A>() them to watch.
 *
 * Move-only: it deregisters its response consumers as it's destroyed.
 */
template <typename TRequest>
class EventRequester
{
  static_assert(detail::checkEvent<TRequest>());
  static_assert(detail::kRequest<TRequest>,
                "An EventRequester asks for a request "
                "([[sfs::event_request(...)]])");

public:
  /** A requester of nothing. */
  EventRequester() = default;
  EventRequester(const EventRequester&) = delete;
  EventRequester& operator=(const EventRequester&) = delete;

  /** Takes over `other`'s response consumers. */
  EventRequester(EventRequester&& other) noexcept
      : m_events(other.m_events), m_owner(other.m_owner), m_who(other.m_who),
        m_channel(std::move(other.m_channel)), m_id(other.m_id),
        m_responses(std::move(other.m_responses))
  {
    other.m_events = nullptr;
    other.m_owner = nullptr;
    other.m_responses.clear();
  }

  /** Lets go of its own response consumers, then takes over `other`'s. */
  EventRequester& operator=(EventRequester&& other) noexcept
  {
    if (this != &other)
    {
      release();
      m_events = other.m_events;
      m_owner = other.m_owner;
      m_who = other.m_who;
      m_channel = std::move(other.m_channel);
      m_id = other.m_id;
      m_responses = std::move(other.m_responses);
      other.m_events = nullptr;
      other.m_owner = nullptr;
      other.m_responses.clear();
    }
    return *this;
  }

  /** Deregisters its response consumers. */
  ~EventRequester() { release(); }

  /**
   * Asks about `key`, superseding the key's current request: its responders
   * stop and its responses are dropped.
   * @param key the entity it's about
   * @param request the request, moved in
   */
  void request(const Entity& key, TRequest request)
  {
    if (live())
      m_events->requestAs(*m_channel, m_id, key, std::move(request));
  }

  /**
   * Whether this requester's current request about the key has no final
   * response yet (Responder::respond()).
   * @param key the entity it asked about
   * @return whether it waits
   */
  bool waiting(const Entity& key) const
  {
    return live() && !m_channel->stale(key) &&
           m_channel->tickets->waiting(m_id, key.getId());
  }

  /**
   * Cancels this requester's current request about the key: its responders
   * stop, its responses are dropped and waiting() turns false. Other
   * requesters' requests about the key stay as they are.
   * @param key the entity
   */
  void cancel(const Entity& key)
  {
    if (live())
      m_events->template cancelAs<TRequest>(*m_channel, m_id, key);
  }

  /**
   * Whether it was made with events to ask through (false for one made by
   * default, refused or moved from).
   * @return whether it asks
   */
  bool valid() const { return m_events != nullptr; }

private:
  template <typename, typename...>
  friend class EventRequesterBuilder;
  friend class Events;
  friend class EventVerbs;

  // Whether its events still live (their channel closes as they go): a
  // requester outliving them asks nothing and waits for nothing.
  bool live() const { return m_events && m_channel && m_channel->open; }

  EventRequester(Events& events,
                 detail::EventOwner* owner,
                 const std::type_info* who)
      : m_events(&events), m_owner(owner), m_who(who)
  {
    m_channel = events.template queue<TRequest>().channel();
    m_id = m_channel->tickets->addRequester();
  }

  // The consumer of R's responses of type A (made the first time), and its
  // handler, run by the owner in its slot.
  template <typename TResponse>
  void addHandler(std::function<void(detail::EventQueueBase&, uint32_t)> run)
  {
    if (!m_events)
      return;
    detail::EventQueue<detail::ResponseTo<TRequest, TResponse>>& queue =
        m_events->template queue<detail::ResponseTo<TRequest, TResponse>>();
    if (!m_owner) [[unlikely]]
    {
      detail::handlersWithoutOwner(queue.name());
      return;
    }
    const detail::ConsumerRef* found = nullptr;
    for (const detail::ConsumerRef& responses : m_responses)
      if (responses.link == queue.link())
        found = &responses;
    if (!found)
    {
      queue.openRoute(m_id,
                      m_who ? detail::readableTypeName(*m_who)
                            : std::string("a requester"));
      const uint32_t slot = queue.addConsumer(m_owner, m_who);
      m_owner->add(queue.link(), slot);
      found =
          &m_responses.emplace_back(detail::ConsumerRef{queue.link(), slot});
    }
    m_owner->addHandler({found->link, found->slot, std::move(run)});
  }

  void release()
  {
    for (const detail::ConsumerRef& responses : m_responses)
      if (detail::EventQueueBase* queue =
              responses.link ? responses.link->queue : nullptr)
      {
        if (detail::EventOwner* owner = queue->consumer(responses.slot).owner)
          owner->forget(responses.link.get(), responses.slot);
        queue->removeConsumer(responses.slot);
        queue->closeRoute(m_id);
      }
    m_responses.clear();
    // Its outstanding requests are superseded (their responders see it). A
    // channel closed already went with its events.
    if (m_channel && m_channel->open)
      m_channel->tickets->removeRequester(m_id);
    m_channel.reset();
    m_events = nullptr;
    m_owner = nullptr;
  }

  Events* m_events = nullptr;
  detail::EventOwner* m_owner = nullptr;
  const std::type_info* m_who = nullptr;
  // The request type's channel, whose tickets hold its requests, and its
  // number there.
  std::shared_ptr<detail::KeyedChannel> m_channel;
  uint32_t m_id = 0;
  // The consumers it made of its response queues.
  std::vector<detail::ConsumerRef> m_responses;
};

/**
 * What requester<R>() gives: an EventRequester<R> being built, which knows
 * the responses handled so far (THandled). Each step handles or ignores one of
 * R's responses and gives the next builder; it becomes the EventRequester<R>
 * it's assigned to only when every one of R's responses is handled or ignored
 * exactly once, checked as it compiles. Handlers are given only here.
 */
template <typename TRequest, typename... THandled>
class [[nodiscard]] EventRequesterBuilder
{
public:
  /**
   * Handles each of R's responses of type A to a current request, called in
   * the owner's slot: `fn(const Entity& key, const A& response)`.
   * @param fn the handler
   * @return the builder with A handled
   */
  template <typename TResponse, typename TFunction>
  EventRequesterBuilder<TRequest, THandled..., TResponse>
  onResponse(TFunction fn) &&
  {
    detail::checkResponseStep<TRequest, TResponse, THandled...>();
    static_assert(std::invocable<TFunction&, const Entity&, const TResponse&>,
                  "onResponse<A>() takes fn(const Entity&, const A&)");
    using Queue = detail::EventQueue<detail::ResponseTo<TRequest, TResponse>>;
    m_requester.template addHandler<TResponse>(
        [fn = std::move(fn), requester = m_requester.m_id](
            detail::EventQueueBase& base, uint32_t slot) mutable
        {
          auto& queue = static_cast<Queue&>(base);
          for (auto [key, response] :
               detail::responseRange<TRequest, TResponse>(
                   queue, slot, requester))
            fn(key, response);
        });
    return EventRequesterBuilder<TRequest, THandled..., TResponse>(
        std::move(m_requester));
  }

  /**
   * Handles each update's responses of type A at once, called in the owner's
   * slot when there are some: `fn(sfs::ResponseRange<R, A>)`, whose rows are
   * `[key, response]`.
   * @param fn the handler
   * @return the builder with A handled
   */
  template <typename TResponse, typename TFunction>
  EventRequesterBuilder<TRequest, THandled..., TResponse>
  onResponses(TFunction fn) &&
  {
    detail::checkResponseStep<TRequest, TResponse, THandled...>();
    static_assert(
        std::invocable<TFunction&, ResponseRange<TRequest, TResponse>>,
        "onResponses<A>() takes fn(sfs::ResponseRange<R, A>)");
    using Queue = detail::EventQueue<detail::ResponseTo<TRequest, TResponse>>;
    m_requester.template addHandler<TResponse>(
        [fn = std::move(fn), requester = m_requester.m_id](
            detail::EventQueueBase& base, uint32_t slot) mutable
        {
          auto& queue = static_cast<Queue&>(base);
          ResponseRange<TRequest, TResponse> responses =
              detail::responseRange<TRequest, TResponse>(
                  queue, slot, requester);
          if (!responses.empty())
            fn(responses);
        });
    return EventRequesterBuilder<TRequest, THandled..., TResponse>(
        std::move(m_requester));
  }

  /**
   * Ignores R's responses of type A, on purpose: this requester gets none.
   * @return the builder with A counted as handled
   */
  template <typename TResponse>
  EventRequesterBuilder<TRequest, THandled..., TResponse> ignore() &&
  {
    detail::checkResponseStep<TRequest, TResponse, THandled...>();
    return EventRequesterBuilder<TRequest, THandled..., TResponse>(
        std::move(m_requester));
  }

  /**
   * The requester, once every one of R's responses is handled or ignored (a
   * compile error names the first that isn't).
   * @return the requester
   */
  operator EventRequester<TRequest>() &&
  {
    detail::AllResponsesHandled<TRequest, THandled...>::check(
        static_cast<typename detail::EventTraits<TRequest>::Responses*>(
            nullptr));
    return std::move(m_requester);
  }

private:
  template <typename, typename...>
  friend class EventRequesterBuilder;
  friend class Events;
  friend class EventVerbs;

  explicit EventRequesterBuilder(EventRequester<TRequest>&& requester)
      : m_requester(std::move(requester))
  {
  }

  EventRequester<TRequest> m_requester;
};

template <typename TEvent>
EventConsumer<TEvent> Events::consumer()
{
  if (!onOwnerThread()) [[unlikely]]
  {
    refuseOffThread<TEvent>("consumer");
    return {};
  }
  return EventConsumer<TEvent>(queue<TEvent>(), m_settingUp, m_settingUpType);
}

template <typename TRequest>
EventRequesterBuilder<TRequest> Events::requester()
{
  if (!onOwnerThread()) [[unlikely]]
  {
    refuseOffThread<TRequest>("requester");
    return EventRequesterBuilder<TRequest>(EventRequester<TRequest>{});
  }
  return EventRequesterBuilder<TRequest>(
      EventRequester<TRequest>(*this, m_settingUp, m_settingUpType));
}

} // namespace sfs
