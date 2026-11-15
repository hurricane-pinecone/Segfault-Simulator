#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/events/eventRules.h"
#include "engine/core/events/events.h"
#include "engine/core/events/keyed.h"
#include "engine/core/events/requestTickets.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace sfs
{

template <typename TEvent, typename TPick, bool kResponders>
class EventRange;

namespace detail
{

/**
 * Fails to compile, naming the request and a response type that isn't in its
 * response list, when a Responder<R> is given one.
 */
template <typename TRequest, typename TResponse>
struct ResponderResponseNotListed
{
  static_assert(sizeof(TResponse) == 0,
                "A Responder<R> sends only the response types R lists; this "
                "one (ResponderResponseNotListed<Request, Response>) isn't "
                "listed");
};

} // namespace detail

/**
 * How a consumer responds to one request of a request type R: what each row
 * and handler of an EventConsumer<R> carries beside the request. It's the
 * request's key, generation and ticket (its requester's request about the
 * key) and the request type's channel: copy it into a job and respond from
 * any thread. It sends the response types R lists
 * (`[[sfs::event_request(A, B)]]`), checked as it compiles; the requester
 * that asked gets them in its response handlers and asks whether the final
 * one is still to come with waiting().
 *
 * Once a newer request from the same requester about the key, a cancel, or
 * the key entity's or the requester's going supersedes this one,
 * superseded() turns true and responses are dropped: a job stops then. The
 * responder a row gives borrows the request type's channel, which lives as
 * long as the events, so a loop (or a handler taking `const Responder<R>&`)
 * that responds at once pays no reference count, even if it destroys the
 * requester first; copying or moving it out of the row (into a job, a list)
 * takes a hold on the channel. A held responder holds the channel, not the
 * events: once they're gone, responses fail. It holds the response types'
 * storage, so it mustn't outlive the code that defines them: a job holding
 * one is cancelled and joined with its scene.
 */
template <typename TRequest>
class Responder
{
  static_assert(detail::checkEvent<TRequest>());
  static_assert(detail::kRequest<TRequest>,
                "A Responder responds to a request "
                "([[sfs::event_request(...)]])");

public:
  /** A responder to nothing: superseded, and every response fails. */
  Responder() = default;

  /**
   * A copy that holds the request type's channel, whether `other`
   * held it or borrowed it from a row.
   * @param other the responder copied
   */
  Responder(const Responder& other)
      : m_channel(other.held()), m_key(other.m_key),
        m_generation(other.m_generation), m_ticket(other.m_ticket)
  {
  }

  /**
   * Takes `other`'s hold on the channel, or a hold of its own if
   * `other` borrowed it from a row. `other` is left a responder to nothing.
   * @param other the responder moved from
   */
  Responder(Responder&& other) noexcept
      : m_channel(other.take()), m_key(other.m_key),
        m_generation(other.m_generation), m_ticket(other.m_ticket)
  {
    other.m_key = {};
    other.m_generation = 0;
    other.m_ticket = 0;
  }

  /** As the copy constructor. */
  Responder& operator=(const Responder& other)
  {
    if (this != &other)
    {
      m_channel = other.held();
      m_key = other.m_key;
      m_generation = other.m_generation;
      m_ticket = other.m_ticket;
    }
    return *this;
  }

  /** As the move constructor. */
  Responder& operator=(Responder&& other) noexcept
  {
    if (this != &other)
    {
      m_channel = other.take();
      m_key = other.m_key;
      m_generation = other.m_generation;
      m_ticket = other.m_ticket;
      other.m_key = {};
      other.m_generation = 0;
      other.m_ticket = 0;
    }
    return *this;
  }

  /**
   * The entity the request is about.
   * @return its key
   */
  const Entity& key() const { return m_key; }

  /**
   * Whether a newer request from the same requester about the key, a
   * cancel, or the key entity's or the requester's going has superseded
   * this request, or its events are gone (three relaxed loads). Any thread.
   * @return whether responses to it are now dropped
   */
  bool superseded() const
  {
    // A closed channel's owner is 0: its events are gone.
    return !m_channel ||
           m_channel->owner.load(std::memory_order_relaxed) == 0 ||
           !m_channel->tickets->current(m_ticket, m_generation);
  }

  /**
   * Sends a partial response, with more to come (a streamed path's next leg).
   * From the owning thread it's readable at once; from a worker, at the next
   * step.
   * @param response one of the response types R lists
   * @return false if the request was superseded or the events are gone
   */
  template <typename TResponse>
  bool progress(TResponse&& response) const
  {
    return respondWith(std::forward<TResponse>(response), false);
  }

  /**
   * Sends the request's final response: the requester's waiting() turns false
   * once it's taken in.
   * @param value one of the response types R lists
   * @return false if the request was superseded or the events are gone
   */
  template <typename TResponse>
  bool respond(TResponse&& value) const
  {
    return respondWith(std::forward<TResponse>(value), true);
  }

private:
  template <typename TEvent, typename TPick, bool kResponders>
  friend class EventRange;

  // A row's responder, borrowing the channel its queue holds, which lives
  // as long as the events: a pointer with no hold (no reference count to
  // change).
  Responder(detail::KeyedChannel& channel,
            const Entity& key,
            uint32_t generation,
            uint32_t ticket)
      : m_channel(std::shared_ptr<detail::KeyedChannel>(), &channel),
        m_key(key), m_generation(generation), m_ticket(ticket)
  {
  }

  // A hold on the channel: this one's, or a new one for a borrowed responder.
  std::shared_ptr<detail::KeyedChannel> held() const
  {
    if (!m_channel || m_channel.use_count() != 0)
      return m_channel;
    return m_channel->shared_from_this();
  }

  // held(), leaving this responder empty.
  std::shared_ptr<detail::KeyedChannel> take() noexcept
  {
    std::shared_ptr<detail::KeyedChannel> out =
        m_channel.use_count() != 0 || !m_channel ? std::move(m_channel)
                                                 : held();
    m_channel.reset();
    return out;
  }

  template <typename TResponse>
  bool respondWith(TResponse&& value, bool last) const
  {
    using Value = std::remove_cvref_t<TResponse>;
    if constexpr (!detail::kListed<
                      Value,
                      typename detail::EventTraits<TRequest>::Responses>)
    {
      [[maybe_unused]] detail::ResponderResponseNotListed<TRequest, Value>
          notListed;
      return false;
    }
    else
      return deliver(
          detail::ResponseTo<TRequest, Value>{std::forward<TResponse>(value)},
          last);
  }

  // Sends a response as its queue holds it: on the owning thread straight to
  // the queue, from a worker to the response inbox.
  template <typename A>
  bool deliver(A&& response, bool last) const
  {
    if (!m_channel)
      return false;
    detail::KeyedChannel& channel = *m_channel;
    // A closed channel's owner is 0. The owning thread's queue checks the
    // request is current itself; a worker checks here.
    const std::uintptr_t owner = channel.owner.load(std::memory_order_relaxed);
    if (owner == 0)
      return false;
    if (owner == detail::currentThreadId())
    {
      Events* events = channel.events;
      return events &&
             events->template queue<A>().respond(
                 std::move(response), m_key, m_generation, last, m_ticket);
    }
    if (!channel.tickets->current(m_ticket, m_generation))
      return false;
    std::shared_ptr<detail::EventInbox<A>> inbox;
    {
      const std::lock_guard lock(channel.mutex);
      if (!channel.open)
        return false;
      inbox = channel.template responseInbox<A>(
          detail::moduleTypeId<A, &detail::eventTypeId>(),
          &Events::template adopt<A>);
    }
    if (!inbox)
      return false;
    const std::lock_guard lock(inbox->mutex);
    if (!inbox->open)
      return false;
    inbox->staged.events.push_back(std::move(response));
    inbox->staged.keys.push_back(m_key);
    inbox->staged.stamps.push_back(m_generation |
                                   (last ? detail::kLastResponse : 0u));
    inbox->staged.tickets.push_back(m_ticket);
    inbox->count.store(static_cast<uint32_t>(inbox->staged.events.size()),
                       std::memory_order_relaxed);
    if (inbox->log) [[unlikely]]
      detail::logWorkerSend(inbox->name);
    return true;
  }

  // The request type's channel: held, or borrowed from a row (no hold:
  // use_count() is 0).
  std::shared_ptr<detail::KeyedChannel> m_channel;
  Entity m_key;
  uint32_t m_generation = 0;
  // The request's ticket (RequestTickets): its requester's about its key.
  uint32_t m_ticket = 0;
};

} // namespace sfs
