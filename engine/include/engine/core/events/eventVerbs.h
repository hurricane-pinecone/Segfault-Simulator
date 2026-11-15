#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/events/eventBroadcast.h"
#include "engine/core/events/eventConsumer.h"
#include "engine/core/events/eventOwner.h"
#include "engine/core/events/eventRules.h"
#include "engine/core/events/events.h"

#include <typeinfo>
#include <utility>

namespace sfs
{

class EventVerbs;

namespace detail
{

/**
 * What a host that runs event handle owners calls: the Registry for its
 * systems, the Scene for its game objects (later a service's host). Handles
 * are made only while their owner sets up; each update is one span, with the
 * owner's handlers run first.
 */
struct EventHost
{
  /**
   * Gives a system the events it sends to and makes handles of.
   * @param user the system
   * @param events its events; null: none
   */
  static inline void attach(EventVerbs& user, Events* events);
  /**
   * A system's set-up (create()) starts or ends: it may make handles only in
   * between.
   * @param user the system
   * @param on whether set-up is starting
   */
  static inline void declaring(EventVerbs& user, bool on);
  /**
   * The owner of handles made with Events::consumer() and requester() while
   * an object sets up (onCreate); null after.
   * @param events the events
   * @param owner the object's owner part; null to end
   * @param who the object's type, for the log; null to end
   */
  static inline void
  settingUp(Events& events, EventOwner* owner, const std::type_info* who);
  /**
   * A system's handle owner part, which its host runs (begin(), dispatch(),
   * end()).
   * @param user the system
   * @return it
   */
  static inline EventOwner& owner(EventVerbs& user);
};

} // namespace detail

/**
 * What a system sends and receives events with (docs/core/events): send()
 * and send(key, event) to tell others something, broadcast<E>() for a worker
 * job's news, and the handles it makes in create() and keeps as members:
 * consumer<E>() to read a type (or give it handlers the engine calls before
 * update()), requester<R>() to ask for a request type and receive its
 * responses. A later event-only base can share it.
 */
class EventVerbs
{
public:
  virtual ~EventVerbs() = default;
  EventVerbs(const EventVerbs&) = delete;
  EventVerbs& operator=(const EventVerbs&) = delete;

  /**
   * Whether any of its handles has events it hasn't read: what a host can
   * check before running an owner that only receives events.
   * @return whether it has unconsumed events
   */
  bool hasUnconsumedEvents() const;

protected:
  EventVerbs() = default;

  /**
   * Sends an unkeyed event (Events::send()).
   * @param event the event, copied or moved in
   */
  template <typename TEvent>
  void send(TEvent&& event)
  {
    if (Events* events = eventsOrLog()) [[likely]]
      events->send(std::forward<TEvent>(event));
  }

  /**
   * Sends a keyed event about `key`, superseding its earlier one for
   * readLatest() (Events::send(key, event)).
   * @param key the entity it's about
   * @param event the event, copied or moved in
   */
  template <typename TEvent>
  void send(const Entity& key, TEvent&& event)
  {
    if (Events* events = eventsOrLog()) [[likely]]
      events->send(key, std::forward<TEvent>(event));
  }

  /**
   * A broadcast of TEvent for a worker job's news (Events::broadcast()).
   * @return the broadcast; one to nowhere without events
   */
  template <typename TEvent>
  EventBroadcast<TEvent> broadcast()
  {
    Events* events = eventsOrLog();
    return events ? events->template broadcast<TEvent>()
                  : EventBroadcast<TEvent>{};
  }

  /**
   * A consumer of TEvent owned by this system, made in create() and kept as
   * a member: registered at once, so nothing sent from then on is lost, and
   * its handlers run before update(). Made elsewhere it is logged and a
   * consumer of nothing is returned.
   * @return the consumer
   */
  template <typename TEvent>
  EventConsumer<TEvent> consumer()
  {
    Events* events = eventsOrLog();
    if (!events) [[unlikely]]
      return {};
    detail::EventQueue<TEvent>& queue = events->template queue<TEvent>();
    if (!m_declaring) [[unlikely]]
    {
      madeOutsideCreate(queue.name(), "consumer");
      return {};
    }
    return EventConsumer<TEvent>(queue, &m_eventOwner, &typeid(*this));
  }

  /**
   * A requester of TRequest owned by this system, made in create() and kept
   * as a member: its builder, which handles (onResponse<A>(), onResponses<A>())
   * or ignores (ignore<A>()) each of the request's responses and is assigned
   * to an EventRequester<R>. Its response handlers run before update(). Made
   * elsewhere it is logged and a requester of nothing is built.
   * @return the requester's builder
   */
  template <typename TRequest>
  EventRequesterBuilder<TRequest> requester()
  {
    using Builder = EventRequesterBuilder<TRequest>;
    Events* events = eventsOrLog();
    if (!events) [[unlikely]]
      return Builder(EventRequester<TRequest>{});
    if (!m_declaring) [[unlikely]]
    {
      madeOutsideCreate(events->template queue<TRequest>().name(), "requester");
      return Builder(EventRequester<TRequest>{});
    }
    return Builder(
        EventRequester<TRequest>(*events, &m_eventOwner, &typeid(*this)));
  }

private:
  friend struct detail::EventHost;

  // The events, or null with an error logged once (a system not yet added
  // to a registry).
  inline Events* eventsOrLog() const
  {
    if (m_events) [[likely]]
      return m_events;
    noEvents();
    return nullptr;
  }

  // A handle made outside create(): logged once, refused.
  void madeOutsideCreate(std::string_view type, const char* kind);
  // An event call with no events to use: logged once.
  void noEvents() const;

  Events* m_events = nullptr;
  detail::EventOwner m_eventOwner;
  bool m_declaring = false;
  bool m_warnedOutside = false;
  mutable bool m_warnedNoEvents = false;
};

namespace detail
{

inline void EventHost::attach(EventVerbs& user, Events* events)
{
  user.m_events = events;
}

inline void EventHost::declaring(EventVerbs& user, bool on)
{
  user.m_declaring = on;
}

inline void EventHost::settingUp(Events& events,
                                 EventOwner* owner,
                                 const std::type_info* who)
{
  events.m_settingUp = owner;
  events.m_settingUpType = who;
}

inline EventOwner& EventHost::owner(EventVerbs& user)
{
  return user.m_eventOwner;
}

} // namespace detail

} // namespace sfs
