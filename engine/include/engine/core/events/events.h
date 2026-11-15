#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/events/eventBroadcast.h"
#include "engine/core/events/eventQueue.h"
#include "engine/core/events/eventRules.h"
#include "engine/core/events/keyed.h"
#include "engine/core/util/typeKey.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace sfs
{

class EventVerbs;
template <typename TRequest>
class Responder;
template <typename TEvent>
class EventConsumer;
template <typename TRequest>
class EventRequester;
template <typename TRequest, typename... THandled>
class EventRequesterBuilder;
namespace detail
{
struct EventHost;
}

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
 * One registry's event queues: what one system tells others: news, commands,
 * requests and responses (docs/core/events). A system sends a typed event; each
 * consumer (an EventConsumer, a system's or an object's) sees every event of
 * its type once, in its owner's update or its handlers just before. Nothing
 * is dispatched the moment it's sent.
 *
 * Each type's queue keeps an event until every consumer registered with it
 * has passed it, so a consumer that reads late or skips frames misses
 * nothing; update() (the frame boundary: after the flush, before the
 * systems) drops what all have passed. A type with no consumers keeps an
 * event for the rest of the frame it was sent in and the whole of the next.
 * A consumer that never consumes is named past 65,536 kept events, and past
 * the cap (setMaxKept()) the oldest are dropped with an error.
 *
 * Threads: the owning thread (the one that made it, rebound by each update())
 * sends, makes consumers and broadcasts, reads and updates. A send() from
 * any other thread is logged (once per type, in every build) and dropped;
 * workers send through an EventBroadcast, whose events are taken in at the
 * next update(), and respond through a Responder.
 *
 * Event types are plain structs marked [[sfs::event_news]]: values only (ids,
 * entities, small vectors or strings moved in), never a pointer into a
 * component or system, since they may be read frames later. A queue is made
 * on a type's first use and keeps the room of the most it has kept: nothing
 * is allocated once warm.
 *
 * Each type is marked with what it is (eventTraits.h): `[[sfs::event_news]]`
 * news, `[[sfs::event_entity_news]]` news about one entity,
 * `[[sfs::event_request(A, B)]]` a request with the responses it gets, or
 * `[[sfs::event_response]]` one of those responses. News that derives from
 * other news is sent to the base's consumers too, as a base. Each consumer
 * reads with read() and readLatest(), or has handlers the engine calls in
 * its owner's slot. A system makes its consumers and requesters in create()
 * (EventVerbs); objects and UI make theirs here.
 */
class Events
{
public:
  Events();
  /**
   * Closes the workers' inboxes and the keyed channels (their broadcasts and
   * responders then fail) and frees every queue.
   */
  ~Events();
  Events(const Events&) = delete;
  Events& operator=(const Events&) = delete;

  /**
   * Sends an event of an unkeyed type: each consumer sees it once, in its
   * next update or read(). On a thread other than the owner's it is logged,
   * once per type, and dropped: use a broadcast there.
   * @param event the event, copied or moved in
   */
  template <typename TEvent>
  void send(TEvent&& event)
  {
    using T = std::remove_cvref_t<TEvent>;
    static_assert(detail::checkEvent<T>());
    static_assert(detail::checkNotResponse<T>());
    static_assert(!detail::kKeyed<T>,
                  "[[sfs::event_entity_news]] is sent with "
                  "send(entity, event)");
    if (!onOwnerThread()) [[unlikely]]
    {
      refuseOffThread<T>("send");
      return;
    }
    queue<T>().push(std::forward<TEvent>(event));
  }

  /**
   * Sends an event of a keyed type that isn't a request, about `key`. It
   * supersedes the key's earlier one for readLatest(); read() still gives
   * every one. Off the owning thread it is logged and dropped.
   * @param key the entity it's about
   * @param event the event, copied or moved in
   */
  template <typename TEvent>
  void send(const Entity& key, TEvent&& event)
  {
    using T = std::remove_cvref_t<TEvent>;
    static_assert(detail::checkEvent<T>());
    static_assert(detail::checkNotResponse<T>());
    static_assert(detail::kKeyed<T>,
                  "send(entity, event) sends an [[sfs::event_entity_news]]; "
                  "[[sfs::event_news]] news is sent with send(event)");
    static_assert(!detail::kRequest<T>,
                  "A request ([[sfs::event_request(...)]]) is sent with a "
                  "requester's request(entity, event)");
    sendKeyed<T>(key, std::forward<TEvent>(event));
  }

  /**
   * Cancels the key's current entity news of TEvent: readLatest() no longer
   * gives it. A request is cancelled through its requester
   * (EventRequester::cancel()). Off the owning thread it is logged and
   * ignored.
   * @param key the entity
   */
  template <typename TEvent>
  void cancel(const Entity& key)
  {
    static_assert(detail::checkEvent<TEvent>());
    static_assert(detail::kOwnChannel<TEvent> && !detail::kRequest<TEvent>,
                  "cancel<E>() cancels an [[sfs::event_entity_news]]'s "
                  "current event; a request is cancelled through its "
                  "requester");
    if (!onOwnerThread()) [[unlikely]]
    {
      refuseOffThread<TEvent>("cancel");
      return;
    }
    detail::EventQueue<TEvent>& events = queue<TEvent>();
    uint32_t generation = 0;
    if (events.channel()->next(key, false, generation) && events.logging())
        [[unlikely]]
      events.logCancelled(key, generation);
  }

  /**
   * A consumer of TEvent for a game object or UI, registered with its queue
   * at the oldest event kept, so nothing sent while a scene sets up is lost
   * to the order things are made in. Make it in the object's onCreate and
   * keep it as a member: its handlers then run before the object's
   * onUpdate, and its reads in an onUpdate share one span. One made
   * elsewhere has no owner: its handlers never run, and each read gives
   * what arrived since the last. A system makes its consumers with its own
   * consumer<E>(), in create(). Off the owning thread it is logged and a
   * consumer of nothing is returned (eventConsumer.h).
   * @return the consumer
   */
  template <typename TEvent>
  EventConsumer<TEvent> consumer();

  /**
   * A requester of TRequest for a game object or UI, made and kept as
   * consumer<E>() is: its builder, which handles or ignores each of the
   * request's responses and is assigned to an EventRequester<R>
   * (eventConsumer.h).
   * @return the requester's builder
   */
  template <typename TRequest>
  EventRequesterBuilder<TRequest> requester();

  /**
   * A broadcast of TEvent, for a worker thread to send through. Take it on
   * the owning thread and give it to the job; on another thread it is logged
   * and a broadcast to nowhere is returned.
   * @return the broadcast
   */
  template <typename TEvent>
  EventBroadcast<TEvent> broadcast()
  {
    static_assert(detail::checkEvent<TEvent>());
    if (!onOwnerThread()) [[unlikely]]
    {
      refuseOffThread<TEvent>("broadcast");
      return {};
    }
    return EventBroadcast<TEvent>(queue<TEvent>().inbox());
  }

  /**
   * The frame boundary: each busy queue takes in the workers' staged events
   * and drops the events every consumer has passed. The calling thread
   * becomes the owner. Scene::update() calls it after the flush; a headless
   * registry's host calls it once a frame.
   */
  void update();

  /**
   * Takes in the responses workers sent since the last call or frame boundary,
   * so a plan that finishes mid-frame is responded at the next step.
   * Scene::update() calls it before each fixed step after the first.
   */
  void takeIn();

  /**
   * An entity is destroyed: its current event of every keyed type, and
   * every requester's request about it, are superseded (responders stop,
   * responses are dropped, waiting() is false), and a keyed send or request
   * through a handle to it is refused from now on, even once a new entity
   * has its id. The registry's flush calls it.
   * @param id the entity's id
   * @param generation the destroyed entity's generation
   */
  void entityRemoved(Entity::EntityId id, uint32_t generation);

  /**
   * Each made queue's counts, in the order the types were first used, for the
   * debug view.
   * @param out cleared, then filled (its room is reused)
   */
  void stats(std::vector<EventStats>& out) const;

  /**
   * Every registered consumer of each made queue, type by type in the order
   * the types were first used, for the debug view.
   * @param out cleared, then filled (its room is reused)
   */
  void consumerStats(std::vector<EventConsumerStats>& out) const;

  /** The owner being set up (EventHost::settingUp()); null for none. */
  inline detail::EventOwner* settingUpOwner() const { return m_settingUp; }
  /** Its type, for the log; null for none. */
  inline const std::type_info* settingUpType() const { return m_settingUpType; }

  /** How many times update() has run: the frame the log names. */
  inline uint64_t frame() const { return m_frame; }

  /**
   * The most events a type keeps for a consumer that doesn't consume them:
   * past it, the oldest are dropped at the frame boundary with an error
   * naming that consumer, and it counts them as missed. Every type, from
   * the next frame boundary.
   * @param events the cap (at least 1)
   */
  void setMaxKept(std::size_t events);

  /** The most events a type keeps (setMaxKept()). */
  inline std::size_t maxKept() const { return m_maxKept; }

  /** The cap on kept events a new Events starts with: 1,048,576 a type. */
  static constexpr std::size_t kDefaultMaxKept = std::size_t{1} << 20;

private:
  friend class EventVerbs;
  template <typename TRequest>
  friend class Responder;
  template <typename TEvent>
  friend class EventConsumer;
  template <typename TRequest>
  friend class EventRequester;
  template <typename TRequest, typename... THandled>
  friend class EventRequesterBuilder;
  friend struct detail::EventHost;

  // T's queue, made on first use.
  template <typename T>
  detail::EventQueue<T>& queue()
  {
    const auto id = static_cast<std::size_t>(
        detail::moduleTypeId<T, &detail::eventTypeId>());
    if (id < m_byId.size() && m_byId[id]) [[likely]]
      return static_cast<detail::EventQueue<T>&>(*m_byId[id]);
    return makeQueue<T>(id);
  }

  // Makes T's queue: a response queue (ResponseTo<R, A>) takes its inbox
  // from its request type's channel and passes responses on to A's own
  // queue; entity news's and a request's channel is kept for the flush.
  template <typename T>
  detail::EventQueue<T>& makeQueue(std::size_t id)
  {
    if constexpr (detail::ResponseQueueType<T>)
    {
      std::shared_ptr<detail::KeyedChannel> requests =
          queue<typename T::Request>().channel();
      detail::EventQueue<typename T::Value>& watchers =
          queue<typename T::Value>();
      auto& made = static_cast<detail::EventQueue<T>&>(
          add(id,
              std::make_unique<detail::EventQueue<T>>(m_frame,
                                                      m_maxKept,
                                                      static_cast<int>(id),
                                                      std::move(requests),
                                                      watchers)));
      m_responseQueues.push_back(&made);
      return made;
    }
    else
    {
      auto& made = static_cast<detail::EventQueue<T>&>(
          add(id, std::make_unique<detail::EventQueue<T>>(m_frame, m_maxKept)));
      linkAncestors(
          made,
          static_cast<typename detail::EventTraits<T>::Ancestors*>(nullptr));
      if constexpr (detail::kOwnChannel<T>)
        addChannel(*made.channel());
      return made;
    }
  }

  // Gives T's queue its event bases' queues, made now if need be, for the
  // copies its sends make.
  template <typename T, typename... TBases>
  void linkAncestors(detail::EventQueue<T>& made, detail::TypeList<TBases...>*)
  {
    std::size_t index = 0;
    (made.setAncestor(index++, queue<TBases>()), ...);
  }

  // A keyed send or request: it starts the key's next generation.
  template <typename T, typename TEvent>
  void sendKeyed(const Entity& key, TEvent&& event)
  {
    if (!onOwnerThread()) [[unlikely]]
    {
      refuseOffThread<T>("send");
      return;
    }
    detail::EventQueue<T>& events = queue<T>();
    uint32_t generation = 0;
    if (events.channel()->next(key, true, generation)) [[likely]]
      events.push(std::forward<TEvent>(event), key, generation);
  }

  // A requester's request: its ticket for the key gets the type's next
  // generation, and the row names the ticket.
  template <typename TRequest>
  void requestAs(detail::KeyedChannel& channel,
                 uint32_t requester,
                 const Entity& key,
                 TRequest&& request)
  {
    using R = std::remove_cvref_t<TRequest>;
    if (!onOwnerThread()) [[unlikely]]
    {
      refuseOffThread<R>("request");
      return;
    }
    if (channel.stale(key)) [[unlikely]]
    {
      channel.refuseStale(key);
      return;
    }
    if (key.getId() >= detail::PagedWords<uint32_t>::kIds) [[unlikely]]
    {
      channel.refuseId(key.getId());
      return;
    }
    uint32_t ticket = 0;
    uint32_t generation = 0;
    if (channel.tickets->ask(requester, key.getId(), ticket, generation))
        [[likely]]
      queue<R>().push(std::forward<TRequest>(request), key, generation, ticket);
  }

  // Cancels a requester's current request about the key.
  template <typename TRequest>
  void
  cancelAs(detail::KeyedChannel& channel, uint32_t requester, const Entity& key)
  {
    if (!onOwnerThread()) [[unlikely]]
    {
      refuseOffThread<TRequest>("cancel");
      return;
    }
    // A stale handle's request went with its entity: nothing to cancel.
    if (channel.stale(key))
      return;
    detail::EventQueue<TRequest>& requests = queue<TRequest>();
    if (channel.tickets->cancel(requester, key.getId()) && requests.logging())
        [[unlikely]]
      requests.logCancelled(key, 0);
  }

  // Makes TResponse's queue for an inbox a worker's response made first.
  template <typename TResponse>
  static void adopt(Events& events)
  {
    events.queue<TResponse>();
  }

  // Keeps a keyed type's channel, for the flush and the owner.
  void addChannel(detail::KeyedChannel& channel);
  // Makes the queues of response types a worker responded first.
  void adoptResponses();
  // Makes the table of entities' least living generations.
  void makeKeyGenerations();

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
  // Every keyed type's channel, in the order made.
  std::vector<detail::KeyedChannel*> m_channels;
  // By entity id: the least generation a living entity with that id can
  // have, set as entities are destroyed; made at the first destroy or keyed
  // type.
  std::unique_ptr<detail::PagedWords<uint32_t>> m_keyGenerations;
  // Every response type's queue: what takeIn() goes through.
  std::vector<detail::EventQueueBase*> m_responseQueues;
  // adoptResponses()'s scratch.
  std::vector<void (*)(Events&)> m_adopting;
  // The owning thread's id. Atomic, so a thread that wrongly calls send()
  // while the owner's update() rebinds it is refused without a data race.
  std::atomic<std::uintptr_t> m_owner{0};
  uint64_t m_frame = 0;
  std::size_t m_maxKept = kDefaultMaxKept;
  // The owner being set up (an object in onCreate), which consumers made
  // here belong to, and its type; set by its host (detail::EventHost).
  detail::EventOwner* m_settingUp = nullptr;
  const std::type_info* m_settingUpType = nullptr;
};

} // namespace sfs

#include "engine/core/events/eventConsumer.h" // IWYU pragma: keep
