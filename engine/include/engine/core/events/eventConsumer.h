#pragma once

#include "engine/core/ecs/entityDeclaration.h"
#include "engine/core/events/eventOwner.h"
#include "engine/core/events/eventQueue.h"
#include "engine/core/events/eventRows.h"
#include "engine/core/events/eventRules.h"
#include "engine/core/events/events.h"
#include "engine/core/events/keyed.h"
#include "engine/core/util/typeKey.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
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

/**
 * Logs a handler given as a member function of a class its handle's owner
 * isn't: it never runs.
 * @param type the event type's name
 * @param handlerClass the member function's class
 * @param owner the handle's owner; null: none of a known type
 */
void handlerOfOtherClass(std::string_view type,
                         const std::type_info& handlerClass,
                         const EventUser* owner);

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

/**
 * A handler given as a member function of its handle's owner, bound to it:
 * calls the member function on the owner with the handler's arguments, as a
 * lambda capturing `this` would. It takes what the member function takes, so
 * the handle checks it as it checks any handler.
 */
template <typename TClass, typename TMember>
struct MemberHandler
{
  static_assert(std::is_function_v<TMember>,
                "A handler given as a member pointer must be a member "
                "function");
  static_assert(std::derived_from<TClass, EventUser>,
                "A handler given as a member function must be a member of "
                "the handle's owner: a class deriving from sfs::System, "
                "sfs::Service or sfs::GameObject");

  /** Calls the member function on the owner. */
  template <typename... TArgs>
    requires std::invocable<TMember TClass::*, TClass*, TArgs...>
  decltype(auto) operator()(TArgs&&... args) const
  {
    return std::invoke(member, object, std::forward<TArgs>(args)...);
  }

  /**
   * The handle's owner, as the member function's class; null when there's
   * none of that class to call it on, and the handler is dropped.
   */
  TClass* object = nullptr;
  /** The member function. */
  TMember TClass::*member = nullptr;
};

/**
 * Binds a member function to a handle's owner, once, as the handler is
 * given: the owner must be a TClass, checked as it's bound.
 * @param owner the handle's owner; null: none
 * @param member the member function
 * @param type the handle's event type's name, for the log
 * @return the handler; its object null when the owner isn't a TClass, or
 *         there's none (logged)
 */
template <typename TClass, typename TMember>
MemberHandler<TClass, TMember> bindMember(const EventOwner* owner,
                                          TMember TClass::*member,
                                          std::string_view type)
{
  MemberHandler<TClass, TMember> handler{nullptr, member};

  if (!owner) [[unlikely]]
  {
    handlersWithoutOwner(type);
    return handler;
  }

  if constexpr (std::derived_from<TClass, EventUser>)
    if (owner->user)
      handler.object = dynamic_cast<TClass*>(owner->user);

  if (!handler.object) [[unlikely]]
    handlerOfOtherClass(type, typeid(TClass), owner->user);

  return handler;
}

/**
 * Whether a handler can be called: any but a member function left with no
 * owner to call it on.
 * @param fn the handler
 * @return whether it can be called
 */
template <typename TFunction>
constexpr bool bound([[maybe_unused]] const TFunction& fn)
{
  return true;
}

/**
 * Whether a member function handler has an owner to call it on.
 * @param handler the handler
 * @return whether it can be called
 */
template <typename TClass, typename TMember>
bool bound(const MemberHandler<TClass, TMember>& handler)
{
  return handler.object != nullptr;
}

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
public:
  /** A consumer of nothing. */
  EventConsumer() = default;
  EventConsumer(const EventConsumer&) = delete;

  /** Takes over `other`'s place, leaving it a consumer of nothing. */
  EventConsumer(EventConsumer&& other) noexcept
      : m_link(std::move(other.m_link)), m_owner(other.m_owner),
        m_slot(other.m_slot)
  {
    other.m_owner = nullptr;
  }

  /** Deregisters: the queue no longer keeps events for it. */
  ~EventConsumer() { release(); }

  EventConsumer& operator=(const EventConsumer&) = delete;

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

  /**
   * Sends the failure response, closing the request as respond() does, to
   * every requester whose current request about `key` is still open, if the
   * request declares one (`failure = F`).
   * @param key the entity the requests are about
   * @param response the request's failure response (copied to each)
   * @return whether any request was open
   */
  template <typename TResponse>
  bool fail(const Entity& key, const TResponse& response)
  {
    return respondOpen<detail::ResponseRole::Failure>(key, response);
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
   * As onEvent(), with a member function of the consumer's owner (the
   * system, service or object that made it) as the handler, taking what
   * onEvent()'s takes, const or not:
   * `m_kills = consumer<EnemyDiedEvent>().onEvent(&ScoreSystem::onKill);`.
   * @param member the owner's member function
   * @return this consumer, for more
   */
  template <typename TClass, typename TMember>
  EventConsumer& onEvent(TMember TClass::*member) &
  {
    addEventHandler(bindToOwner(member));
    return *this;
  }
  /** As onEvent(member) on a consumer just made, to be assigned. */
  template <typename TClass, typename TMember>
  EventConsumer&& onEvent(TMember TClass::*member) &&
  {
    addEventHandler(bindToOwner(member));
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
   * As onEvents(), with a member function of the consumer's owner as the
   * handler: `void Owner::fn(sfs::EventRange<E>)`, const or not.
   * @param member the owner's member function
   * @return this consumer, for more
   */
  template <typename TClass, typename TMember>
  EventConsumer& onEvents(TMember TClass::*member) &
  {
    addEventsHandler(bindToOwner(member));
    return *this;
  }
  /** As onEvents(member) on a consumer just made, to be assigned. */
  template <typename TClass, typename TMember>
  EventConsumer&& onEvents(TMember TClass::*member) &&
  {
    addEventsHandler(bindToOwner(member));
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
   * As onLatest(), with a member function of the consumer's owner as the
   * handler, taking what onLatest()'s takes, const or not.
   * @param member the owner's member function
   * @return this consumer, for more
   */
  template <typename TClass, typename TMember>
  EventConsumer& onLatest(TMember TClass::*member) &
  {
    addLatestHandler(bindToOwner(member));
    return *this;
  }
  /** As onLatest(member) on a consumer just made, to be assigned. */
  template <typename TClass, typename TMember>
  EventConsumer&& onLatest(TMember TClass::*member) &&
  {
    addLatestHandler(bindToOwner(member));
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
   * As onLatestEvents(), with a member function of the consumer's owner as
   * the handler: `void Owner::fn(sfs::LatestEventRange<E>)`, const or not.
   * @param member the owner's member function
   * @return this consumer, for more
   */
  template <typename TClass, typename TMember>
  EventConsumer& onLatestEvents(TMember TClass::*member) &
  {
    addLatestEventsHandler(bindToOwner(member));
    return *this;
  }
  /** As onLatestEvents(member) on a consumer just made, to be assigned. */
  template <typename TClass, typename TMember>
  EventConsumer&& onLatestEvents(TMember TClass::*member) &&
  {
    addLatestEventsHandler(bindToOwner(member));
    return std::move(*this);
  }

  /**
   * Whether a request of R about `key` is still open: current, from a
   * requester that lives, with no final response taken in yet (a worker's
   * comes in at the next frame step). Owning thread only: off it, logged
   * and false.
   * @param key the entity
   * @return whether one is open
   */
  bool open(const Entity& key) const
  {
    static_assert(detail::kRequest<TEvent>,
                  "open(entity) asks a request type's consumer "
                  "([[sfs::event_request(...)]])");
    const detail::EventQueue<TEvent>* queue = this->queue();

    if (!queue)
      return false;

    const detail::KeyedChannel& channel = *queue->channel();

    if (channel.owner.load(std::memory_order_relaxed) !=
        detail::currentThreadId()) [[unlikely]]
    {
      Events::refuseOffThread<TEvent>("open", true);
      return false;
    }

    if (channel.stale(key))
      return false;

    return channel.tickets->open(key.getId());
  }

  /**
   * Sends a progress response, with more to come, to every requester whose
   * current request about `key` is still open, as respond() does, if the
   * request declares one (`progress = P`).
   * @param key the entity the requests are about
   * @param response the request's progress response (copied to each)
   * @return whether any request was open
   */
  template <typename TResponse>
  bool progress(const Entity& key, const TResponse& response)
  {
    return respondOpen<detail::ResponseRole::Progress>(key, response);
  }

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
   * Sends the success response to every requester whose current request
   * about `key` is still open: a server answering later, by entity, holding
   * no Responder ("tell me once you're finished"). Each requester hears only
   * its own; a replaced, cancelled or gone request isn't reached; the
   * request closes, so waiting() and open() turn false. Owning thread only
   * (a job responds through a Responder).
   * @param key the entity the requests are about
   * @param response the request's success response (copied to each)
   * @return whether any request was open
   */
  template <typename TResponse>
  bool respond(const Entity& key, const TResponse& response)
  {
    return respondOpen<detail::ResponseRole::Success>(key, response);
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

  /**
   * Whether it's registered with a queue that lives (false for one made by
   * default, refused, moved from, or outliving its events).
   * @return whether it reads
   */
  bool valid() const { return queueBase() != nullptr; }

private:
  static_assert(detail::checkEvent<TEvent>());
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

    if (!detail::bound(fn))
      return;

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

    if (!detail::bound(fn))
      return;

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

  template <typename TFunction>
  void addLatestEventsHandler(TFunction fn)
  {
    static_assert(detail::kOwnChannel<TEvent>,
                  "onLatestEvents() takes an [[sfs::event_entity_news]] or a "
                  "request");
    static_assert(std::invocable<TFunction&, LatestEventRange<TEvent>>,
                  "onLatestEvents() takes fn(sfs::LatestEventRange<E>)");

    if (!detail::bound(fn))
      return;

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

    if (!detail::bound(fn))
      return;

    addHandler(
        [fn = std::move(fn)](
            detail::EventQueueBase& base, uint32_t slot) mutable
        {
          auto& queue = static_cast<detail::EventQueue<TEvent>&>(base);

          for (auto&& row : detail::spanRange<TEvent, true>(queue, slot))
            detail::callWithRow<TEvent>(fn, row);
        });
  }

  // A member function of the consumer's owner, bound to it; one with no
  // owner to call it on (a consumer of nothing has none) is dropped.
  template <typename TClass, typename TMember>
  detail::MemberHandler<TClass, TMember>
  bindToOwner(TMember TClass::*member) const
  {
    const detail::EventQueueBase* queue = queueBase();

    if (!queue)
      return {nullptr, member};

    return detail::bindMember(m_owner, member, queue->name());
  }

  detail::EventQueue<TEvent>* queue() const
  {
    return static_cast<detail::EventQueue<TEvent>*>(queueBase());
  }

  detail::EventQueueBase* queueBase() const
  {
    return m_link ? m_link->queue : nullptr;
  }

  // Frees its slot and its handlers. The owner is the slot's, which its
  // owner clears as it goes, so a consumer outliving its owner is safe.
  void release()
  {
    if (!m_link)
      return;

    if (detail::EventQueueBase* queue = m_link->queue)
    {
      detail::EventOwner* owner = queue->consumer(m_slot).owner;
      queue->removeConsumer(m_slot);

      if (owner)
        owner->forget();
    }

    m_link.reset();
    m_owner = nullptr;
  }

  // respond(), progress() and fail(): the response to each open request
  // about the key, through its response queue, as a Responder would on this
  // thread.
  template <detail::ResponseRole kRole, typename TResponse>
  bool respondOpen(const Entity& key, const TResponse& response)
  {
    static_assert(detail::kRequest<TEvent>,
                  "respond(), progress() and fail() by entity answer through "
                  "a request type's consumer ([[sfs::event_request(...)]])");
    using Value = std::remove_cvref_t<TResponse>;
    constexpr bool LAST = kRole != detail::ResponseRole::Progress;
    static_assert(std::is_copy_constructible_v<Value>,
                  "An answer by entity is copied to each open request about "
                  "it, so its response type must be copyable; answer a "
                  "move-only response through the request's Responder");

    if constexpr (!detail::kRequest<TEvent> ||
                  !std::is_copy_constructible_v<Value>)
      return false;
    else if constexpr (!detail::checkRoleSent<TEvent, kRole, Value>() ||
                       !std::is_same_v<detail::RoleType<TEvent, kRole>, Value>)
      return false;
    else
    {
      detail::EventQueue<TEvent>* queue = this->queue();

      if (!queue)
        return false;

      detail::KeyedChannel& channel = *queue->channel();

      if (channel.owner.load(std::memory_order_relaxed) !=
          detail::currentThreadId()) [[unlikely]]
      {
        constexpr const char* CALL =
            kRole == detail::ResponseRole::Success    ? "respond"
            : kRole == detail::ResponseRole::Progress ? "progress"
                                                      : "fail";
        Events::refuseOffThread<TEvent>(CALL, true);
        return false;
      }

      if (!channel.events || channel.stale(key))
        return false;

      auto& responses =
          channel.events->template queue<detail::ResponseTo<TEvent, Value>>();
      bool reached = false;
      channel.tickets->forEachOpen(
          key.getId(),
          [&](uint32_t ticket, uint32_t generation)
          {
            reached |=
                responses.respond(detail::ResponseTo<TEvent, Value>{response},
                                  key,
                                  generation,
                                  LAST,
                                  ticket);
          });
      return reached;
    }
  }

  std::shared_ptr<detail::EventQueueLink> m_link;
  detail::EventOwner* m_owner = nullptr;
  uint32_t m_slot = 0;
};

/**
 * What a service's consumer<E>() gives: an EventConsumer<E> being built,
 * which becomes the consumer it's assigned to only once it has a handler
 * (onEvent(), onEvents(), onLatest(), onLatestEvents(), each as the
 * consumer's, a callable or a member function of the service), checked as it
 * compiles: an owner with no update to read in runs only its handlers. It is
 * moved, never copied.
 */
template <typename TEvent, bool kHandled = false>
class [[nodiscard]] EventConsumerBuilder
{
public:
  /**
   * Builds on a consumer just made.
   * @param consumer the consumer, moved in
   */
  explicit EventConsumerBuilder(EventConsumer<TEvent>&& consumer)
      : m_consumer(std::move(consumer))
  {
  }

  /**
   * Gives it a handler for each new event (EventConsumer::onEvent()).
   * @param fn the handler
   * @return the builder, handled
   */
  template <typename TFunction>
  EventConsumerBuilder<TEvent, true> onEvent(TFunction fn) &&
  {
    m_consumer.onEvent(std::move(fn));
    return EventConsumerBuilder<TEvent, true>(std::move(m_consumer));
  }

  /**
   * Gives it a handler for each run's new events at once
   * (EventConsumer::onEvents()).
   * @param fn the handler
   * @return the builder, handled
   */
  template <typename TFunction>
  EventConsumerBuilder<TEvent, true> onEvents(TFunction fn) &&
  {
    m_consumer.onEvents(std::move(fn));
    return EventConsumerBuilder<TEvent, true>(std::move(m_consumer));
  }

  /**
   * Gives it a handler for each key's newest current event among the new
   * ones (EventConsumer::onLatest()).
   * @param fn the handler
   * @return the builder, handled
   */
  template <typename TFunction>
  EventConsumerBuilder<TEvent, true> onLatest(TFunction fn) &&
  {
    m_consumer.onLatest(std::move(fn));
    return EventConsumerBuilder<TEvent, true>(std::move(m_consumer));
  }

  /**
   * Gives it a handler for each run's newest current events per key at once
   * (EventConsumer::onLatestEvents()).
   * @param fn the handler
   * @return the builder, handled
   */
  template <typename TFunction>
  EventConsumerBuilder<TEvent, true> onLatestEvents(TFunction fn) &&
  {
    m_consumer.onLatestEvents(std::move(fn));
    return EventConsumerBuilder<TEvent, true>(std::move(m_consumer));
  }

  /**
   * The consumer built, once it has a handler.
   * @return the consumer
   */
  operator EventConsumer<TEvent>() &&
  {
    static_assert(kHandled,
                  "A Service's consumer needs a handler (onEvent(), "
                  "onEvents(), onLatest() or onLatestEvents()): a Service "
                  "has no update() to read it in");
    return std::move(m_consumer);
  }

private:
  EventConsumer<TEvent> m_consumer;
};

namespace detail
{

/** Fails to compile, naming a request whose success isn't handled. */
template <typename TRequest>
struct SuccessNotHandled
{
  static_assert(sizeof(TRequest) == 0,
                "A request's success response (SuccessNotHandled<Request>) "
                "has no handler; give the requester onResponse() or "
                "onResponses()");
};

/** Fails to compile, naming a request whose failure isn't handled. */
template <typename TRequest>
struct FailureNotHandled
{
  static_assert(sizeof(TRequest) == 0,
                "A request's failure response (FailureNotHandled<Request>) "
                "has no handler; give the requester onFailure() or "
                "onFailures(), or .ignoreFailure()");
};

/** Fails to compile, naming a request whose role is handled twice. */
template <typename TRequest>
struct RoleHandledTwice
{
  static_assert(sizeof(TRequest) == 0,
                "A requester handles each of the request's roles once; this "
                "one (RoleHandledTwice<Request>) is handled twice");
};

/** Fails to compile, naming a request with no progress role to handle. */
template <typename TRequest>
struct NoProgressToHandle
{
  static_assert(sizeof(TRequest) == 0,
                "This request (NoProgressToHandle<Request>) declares no "
                "progress response, so there's none for onProgress() or "
                "onProgresses() to handle");
};

/** Fails to compile, naming a request with no failure role to handle. */
template <typename TRequest>
struct NoFailureToHandle
{
  static_assert(sizeof(TRequest) == 0,
                "This request (NoFailureToHandle<Request>) declares no "
                "failure response, so there's none for onFailure(), "
                "onFailures() or ignoreFailure()");
};

// A requester builder's step for a role: declared, and not handled yet.
template <typename TRequest, ResponseRole kRole, bool kHandled>
constexpr void checkRoleStep()
{
  using Declared = RoleType<TRequest, kRole>;

  if constexpr (kRole == ResponseRole::Progress && std::is_void_v<Declared>)
    static_cast<void>(sizeof(NoProgressToHandle<TRequest>));
  else if constexpr (kRole == ResponseRole::Failure && std::is_void_v<Declared>)
    static_cast<void>(sizeof(NoFailureToHandle<TRequest>));
  else if constexpr (kHandled)
    static_cast<void>(sizeof(RoleHandledTwice<TRequest>));
}

} // namespace detail

/**
 * Asks for a request type R ([[sfs::event_request(...)]]) and receives its
 * responses: a system makes one in create() and keeps it as a member (an
 * object in onCreate, through scene().events()), built in one chain that
 * handles R's responses by role: its success (required), its failure if it
 * declares one (or ignores it on purpose), its progress if it wants it:
 *
 *   m_paths = requester<PathRequest>()
 *                 .onResponse(...)
 *                 .onProgress(...)
 *                 .ignoreFailure();
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
public:
  /** A requester of nothing. */
  EventRequester() = default;
  EventRequester(const EventRequester&) = delete;

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

  /** Deregisters its response consumers. */
  ~EventRequester() { release(); }

  EventRequester& operator=(const EventRequester&) = delete;

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
   * Whether it was made with events to ask through (false for one made by
   * default, refused or moved from).
   * @return whether it asks
   */
  bool valid() const { return m_events != nullptr; }

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

private:
  static_assert(detail::checkEvent<TRequest>());
  static_assert(detail::kRequest<TRequest>,
                "An EventRequester asks for a request "
                "([[sfs::event_request(...)]])");
  template <typename, bool, bool, bool>
  friend class EventRequesterBuilder;
  friend class Events;
  friend class EventVerbs;

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

  // Whether its events still live (their channel closes as they go): a
  // requester outliving them asks nothing and waits for nothing.
  bool live() const { return m_events && m_channel && m_channel->open; }

  void release()
  {
    for (const detail::ConsumerRef& responses : m_responses)
      if (detail::EventQueueBase* queue =
              responses.link ? responses.link->queue : nullptr)
      {
        detail::EventOwner* owner = queue->consumer(responses.slot).owner;
        queue->removeConsumer(responses.slot);
        queue->closeRoute(m_id);

        if (owner)
          owner->forget();
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
 * the roles handled so far. Each step handles one of R's roles (a handler
 * for one response at a time, or for the update's at once) and gives the
 * next builder; it becomes the EventRequester<R> it's assigned to only when
 * R's success is handled, and its failure (if R declares one) handled or
 * ignored, checked as it compiles. Progress is optional: a requester that
 * doesn't handle it gets none. Handlers are given only here, and the owner
 * gets them in role order as the requester is made: progress, then success
 * and failure, so a request's progress runs before its final in an update
 * whatever order the chain gives them.
 */
template <typename TRequest, bool kSuccess, bool kProgress, bool kFailure>
class [[nodiscard]] EventRequesterBuilder
{
public:
  /**
   * Ignores R's failure response, on purpose: this requester gets none.
   * @return the builder with the failure counted as handled
   */
  EventRequesterBuilder<TRequest, kSuccess, kProgress, true> ignoreFailure() &&
  {
    detail::checkRoleStep<TRequest, detail::ResponseRole::Failure, kFailure>();
    return next<kSuccess, kProgress, true>();
  }

  /**
   * Handles each failure response (a request declaring `failure = F`):
   * `fn(const Entity& key, const F& response)`.
   * @param fn the handler
   * @return the builder with the failure handled
   */
  template <typename TFunction>
  EventRequesterBuilder<TRequest, kSuccess, kProgress, true>
  onFailure(TFunction fn) &&
  {
    detail::checkRoleStep<TRequest, detail::ResponseRole::Failure, kFailure>();

    if constexpr (!std::is_void_v<Failure>)
      addOne<detail::ResponseRole::Failure>(std::move(fn));

    return next<kSuccess, kProgress, true>();
  }

  /**
   * As onFailure(), with a member function of the requester's owner as the
   * handler: `void Owner::fn(const Entity& key, const F& response)`, const
   * or not.
   * @param member the owner's member function
   * @return the builder, as onFailure()'s
   */
  template <typename TClass, typename TMember>
  EventRequesterBuilder<TRequest, kSuccess, kProgress, true>
  onFailure(TMember TClass::*member) &&
  {
    return std::move(*this).onFailure(bindToOwner(member));
  }

  /**
   * Handles each update's failure responses at once:
   * `fn(sfs::ResponseRange<R, F>)`.
   * @param fn the handler
   * @return the builder with the failure handled
   */
  template <typename TFunction>
  EventRequesterBuilder<TRequest, kSuccess, kProgress, true>
  onFailures(TFunction fn) &&
  {
    detail::checkRoleStep<TRequest, detail::ResponseRole::Failure, kFailure>();

    if constexpr (!std::is_void_v<Failure>)
      addBatch<detail::ResponseRole::Failure>(std::move(fn));

    return next<kSuccess, kProgress, true>();
  }

  /**
   * As onFailures(), with a member function of the requester's owner as the
   * handler: `void Owner::fn(sfs::ResponseRange<R, F>)`, const or not.
   * @param member the owner's member function
   * @return the builder, as onFailures()'s
   */
  template <typename TClass, typename TMember>
  EventRequesterBuilder<TRequest, kSuccess, kProgress, true>
  onFailures(TMember TClass::*member) &&
  {
    return std::move(*this).onFailures(bindToOwner(member));
  }

  /**
   * Handles each progress response (a request declaring `progress = P`):
   * `fn(const Entity& key, const P& response)`.
   * @param fn the handler
   * @return the builder with progress handled
   */
  template <typename TFunction>
  EventRequesterBuilder<TRequest, kSuccess, true, kFailure>
  onProgress(TFunction fn) &&
  {
    detail::
        checkRoleStep<TRequest, detail::ResponseRole::Progress, kProgress>();

    if constexpr (!std::is_void_v<Progress>)
      addOne<detail::ResponseRole::Progress>(std::move(fn));

    return next<kSuccess, true, kFailure>();
  }

  /**
   * As onProgress(), with a member function of the requester's owner as the
   * handler: `void Owner::fn(const Entity& key, const P& response)`, const
   * or not.
   * @param member the owner's member function
   * @return the builder, as onProgress()'s
   */
  template <typename TClass, typename TMember>
  EventRequesterBuilder<TRequest, kSuccess, true, kFailure>
  onProgress(TMember TClass::*member) &&
  {
    return std::move(*this).onProgress(bindToOwner(member));
  }

  /**
   * Handles each update's progress responses at once:
   * `fn(sfs::ResponseRange<R, P>)`.
   * @param fn the handler
   * @return the builder with progress handled
   */
  template <typename TFunction>
  EventRequesterBuilder<TRequest, kSuccess, true, kFailure>
  onProgresses(TFunction fn) &&
  {
    detail::
        checkRoleStep<TRequest, detail::ResponseRole::Progress, kProgress>();

    if constexpr (!std::is_void_v<Progress>)
      addBatch<detail::ResponseRole::Progress>(std::move(fn));

    return next<kSuccess, true, kFailure>();
  }

  /**
   * As onProgresses(), with a member function of the requester's owner as
   * the handler: `void Owner::fn(sfs::ResponseRange<R, P>)`, const or not.
   * @param member the owner's member function
   * @return the builder, as onProgresses()'s
   */
  template <typename TClass, typename TMember>
  EventRequesterBuilder<TRequest, kSuccess, true, kFailure>
  onProgresses(TMember TClass::*member) &&
  {
    return std::move(*this).onProgresses(bindToOwner(member));
  }

  /**
   * Handles each success response to a current request, in the owner's
   * slot: `fn(const Entity& key, const Success& response)`.
   * @param fn the handler
   * @return the builder with the success handled
   */
  template <typename TFunction>
  EventRequesterBuilder<TRequest, true, kProgress, kFailure>
  onResponse(TFunction fn) &&
  {
    detail::checkRoleStep<TRequest, detail::ResponseRole::Success, kSuccess>();
    addOne<detail::ResponseRole::Success>(std::move(fn));
    return next<true, kProgress, kFailure>();
  }

  /**
   * As onResponse(), with a member function of the requester's owner (the
   * system, service or object that made it) as the handler:
   * `void Owner::fn(const Entity& key, const Success& response)`, const or
   * not: `requester<PathRequest>().onResponse(&EnemyAI::onPath)`.
   * @param member the owner's member function
   * @return the builder, as onResponse()'s
   */
  template <typename TClass, typename TMember>
  EventRequesterBuilder<TRequest, true, kProgress, kFailure>
  onResponse(TMember TClass::*member) &&
  {
    return std::move(*this).onResponse(bindToOwner(member));
  }

  /**
   * Handles each update's success responses at once, in the owner's slot
   * when there are some: `fn(sfs::ResponseRange<R, Success>)`.
   * @param fn the handler
   * @return the builder with the success handled
   */
  template <typename TFunction>
  EventRequesterBuilder<TRequest, true, kProgress, kFailure>
  onResponses(TFunction fn) &&
  {
    detail::checkRoleStep<TRequest, detail::ResponseRole::Success, kSuccess>();
    addBatch<detail::ResponseRole::Success>(std::move(fn));
    return next<true, kProgress, kFailure>();
  }

  /**
   * As onResponses(), with a member function of the requester's owner as
   * the handler: `void Owner::fn(sfs::ResponseRange<R, Success>)`, const or
   * not.
   * @param member the owner's member function
   * @return the builder, as onResponses()'s
   */
  template <typename TClass, typename TMember>
  EventRequesterBuilder<TRequest, true, kProgress, kFailure>
  onResponses(TMember TClass::*member) &&
  {
    return std::move(*this).onResponses(bindToOwner(member));
  }

  /**
   * The requester, once R's success is handled and its failure handled or
   * ignored (a compile error names what isn't).
   * @return the requester
   */
  operator EventRequester<TRequest>() &&
  {
    if constexpr (!kSuccess)
      static_cast<void>(sizeof(detail::SuccessNotHandled<TRequest>));

    if constexpr (!std::is_void_v<Failure> && !kFailure)
      static_cast<void>(sizeof(detail::FailureNotHandled<TRequest>));

    give<detail::ResponseRole::Progress>();
    give<detail::ResponseRole::Success>();
    give<detail::ResponseRole::Failure>();
    return std::move(m_requester);
  }

private:
  using Success = detail::RoleType<TRequest, detail::ResponseRole::Success>;
  using Progress = detail::RoleType<TRequest, detail::ResponseRole::Progress>;
  using Failure = detail::RoleType<TRequest, detail::ResponseRole::Failure>;
  template <typename, bool, bool, bool>
  friend class EventRequesterBuilder;
  friend class Events;
  friend class EventVerbs;

  // A handler's body, run by the owner over its consumer's span.
  using Run = std::function<void(detail::EventQueueBase&, uint32_t)>;
  // The handlers given so far, by role.
  using Runs = std::array<Run, 3>;

  explicit EventRequesterBuilder(EventRequester<TRequest>&& requester,
                                 Runs&& runs = {})
      : m_requester(std::move(requester)), m_runs(std::move(runs))
  {
  }

  // A handler for each update's responses of the role at once.
  template <detail::ResponseRole kRole, typename TFunction>
  void addBatch(TFunction fn)
  {
    using TResponse = detail::RoleType<TRequest, kRole>;
    static_assert(
        std::invocable<TFunction&, ResponseRange<TRequest, TResponse>>,
        "onResponses(), onProgresses() and onFailures() take "
        "fn(sfs::ResponseRange<R, Response>), the role's response");

    if (!detail::bound(fn))
      return;

    using Queue = detail::EventQueue<detail::ResponseTo<TRequest, TResponse>>;
    m_runs[static_cast<std::size_t>(kRole)] =
        [fn = std::move(fn), requester = m_requester.m_id](
            detail::EventQueueBase& base, uint32_t slot) mutable
    {
      auto& queue = static_cast<Queue&>(base);
      ResponseRange<TRequest, TResponse> responses =
          detail::responseRange<TRequest, TResponse>(queue, slot, requester);

      if (!responses.empty())
        fn(responses);
    };
  }

  // A handler for each response of the role, unpacked.
  template <detail::ResponseRole kRole, typename TFunction>
  void addOne(TFunction fn)
  {
    using TResponse = detail::RoleType<TRequest, kRole>;
    static_assert(std::invocable<TFunction&, const Entity&, const TResponse&>,
                  "onResponse(), onProgress() and onFailure() take "
                  "fn(const Entity&, const Response&), the role's response");

    if (!detail::bound(fn))
      return;

    using Queue = detail::EventQueue<detail::ResponseTo<TRequest, TResponse>>;
    m_runs[static_cast<std::size_t>(kRole)] =
        [fn = std::move(fn), requester = m_requester.m_id](
            detail::EventQueueBase& base, uint32_t slot) mutable
    {
      auto& queue = static_cast<Queue&>(base);

      for (auto [key, response] :
           detail::responseRange<TRequest, TResponse>(queue, slot, requester))
        fn(key, response);
    };
  }

  // A member function of the requester's owner, bound to it; one with no
  // owner to call it on (a requester of nothing has none) is dropped.
  template <typename TClass, typename TMember>
  detail::MemberHandler<TClass, TMember> bindToOwner(TMember TClass::*member)
  {
    if (!m_requester.m_events)
      return {nullptr, member};

    return detail::bindMember(
        m_requester.m_owner,
        member,
        m_requester.m_events->template queue<TRequest>().name());
  }

  // Gives the owner the role's handler, if there is one.
  template <detail::ResponseRole kRole>
  void give()
  {
    using Response = detail::RoleType<TRequest, kRole>;

    if constexpr (!std::is_void_v<Response>)
      if (Run& run = m_runs[static_cast<std::size_t>(kRole)])
        m_requester.template addHandler<Response>(std::move(run));
  }

  template <bool kNextSuccess, bool kNextProgress, bool kNextFailure>
  EventRequesterBuilder<TRequest, kNextSuccess, kNextProgress, kNextFailure>
  next()
  {
    return EventRequesterBuilder<TRequest,
                                 kNextSuccess,
                                 kNextProgress,
                                 kNextFailure>(
        std::move(m_requester), std::move(m_runs));
  }

  EventRequester<TRequest> m_requester;
  Runs m_runs;
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
