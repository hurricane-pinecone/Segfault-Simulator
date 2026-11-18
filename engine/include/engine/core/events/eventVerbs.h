#pragma once

#include "engine/core/ecs/entityDeclaration.h"
#include "engine/core/events/eventBroadcast.h"
#include "engine/core/events/eventConsumer.h"
#include "engine/core/events/eventOwner.h"
#include "engine/core/events/eventQueue.h"
#include "engine/core/events/events.h"

#include <string_view>
#include <typeinfo>
#include <utility>

namespace sfs
{

class EventVerbs;

namespace detail
{

/**
 * What a host that runs event handle owners calls: the Registry for its
 * systems and services, the Scene for its game objects. Handles
 * are made only while their owner sets up; each update is one span, with the
 * owner's handlers run first.
 */
struct EventHost
{
  /**
   * Gives a system or service the events it sends to and makes handles of.
   * @param user the system or service
   * @param events its events; null: none
   */
  static inline void attach(EventVerbs& user, Events* events);
  /**
   * A system's or service's set-up (create()) starts or ends: it may make
   * handles only in between.
   * @param user the system or service
   * @param on whether set-up is starting
   */
  static inline void declaring(EventVerbs& user, bool on);
  /**
   * A system's or service's handle owner part, which its host runs (begin(),
   * dispatch(), end()).
   * @param user the system or service
   * @return it
   */
  static inline EventOwner& owner(EventVerbs& user);
  /**
   * The owner of handles made with Events::consumer() and requester() while
   * an object sets up (onCreate); null after.
   * @param events the events
   * @param owner the object's owner part; null to end
   * @param who the object's type, for the log; null to end
   */
  static inline void
  settingUp(Events& events, EventOwner* owner, const std::type_info* who);
};

} // namespace detail

/**
 * What a system or a service sends and receives events with
 * (docs/core/events): send() and send(key, event) to tell others something,
 * cancel<E>(key) to take back entity news, broadcast<E>() for a worker job's
 * news, and the handles it makes in create() and keeps as members:
 * consumer<E>() to read a type (or give it handlers the engine calls before
 * update()), requester<R>() to ask for a request type and receive its
 * responses.
 */
class EventVerbs : public detail::EventUser
{
public:
  EventVerbs(const EventVerbs&) = delete;
  ~EventVerbs() override = default;
  EventVerbs& operator=(const EventVerbs&) = delete;
  /**
   * Whether it runs: a disabled system or service is skipped by its host
   * (and by render providers), its handles keeping their places so it
   * misses nothing. For debug toggling; the render system is never disabled.
   * @return whether it's enabled
   */
  bool enabled() const { return m_enabled; }
  /**
   * Whether any of its handles has events it hasn't read: what a host can
   * check before running an owner that only receives events.
   * @return whether it has unconsumed events
   */
  bool hasUnconsumedEvents() const;
  /**
   * Enables or disables it (see enabled()).
   * @param enabled whether it runs
   */
  inline void setEnabled(bool enabled)
  {
    m_enabled = enabled;

    if (!enabled)
      m_eventOwner.wasDisabled = true;
  }

protected:
  EventVerbs() = default;

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
   * Cancels the key's current entity news of TEvent, so readLatest() no
   * longer gives it (Events::cancel()).
   * @param key the entity it's about
   */
  template <typename TEvent>
  void cancel(const Entity& key)
  {
    if (Events* events = eventsOrLog()) [[likely]]
      events->template cancel<TEvent>(key);
  }

  /**
   * A consumer of TEvent owned by this system or service, made in create()
   * and kept as a member: registered at once, so nothing sent from then on
   * is lost, and its handlers run before update() (a service's whenever it
   * runs). Made elsewhere it is logged and a consumer of nothing is
   * returned.
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
   * A requester of TRequest owned by this system or service, made in
   * create() and kept as a member: its builder, which handles the request's
   * responses by role (onResponse(), onProgress(), onFailure() and their
   * batch forms, or ignoreFailure()) and is assigned to an
   * EventRequester<R>. Its response handlers run before update() (a
   * service's whenever it runs). Made elsewhere it is logged and a
   * requester of nothing is built.
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
  detail::EventOwner m_eventOwner{this};
  bool m_declaring = false;
  bool m_enabled = true;
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
