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

/**
 * How a consumer responds to one request of a request type R: what each row
 * and handler of an EventConsumer<R> carries beside the request. It's the
 * request's key, generation and ticket (its requester's request about the
 * key) and the request type's channel: copy it into a job and respond from
 * any thread. It sends R's responses by their role
 * (`[[sfs::event_request(Success, progress = P, failure = F)]]`): respond()
 * the success, progress() a progress, fail() the failure, each checked as it
 * compiles. The requester that asked gets them in its handlers
 * (onResponse(), onProgress(), onFailure()) and asks whether a final one is
 * still to come with waiting().
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
   * A request already closed by a final response (answered by entity before
   * this responder's row was read, say) isn't superseded, but its responses
   * are refused.
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
   * Sends the request's success response, its final answer: the request
   * closes once it's taken in, and a later final is refused. From the
   * owning thread it's taken in at once; from a worker, at the next step,
   * so until then the request stays open and a final sent on the owning
   * thread first wins (the worker's is dropped).
   * @param value the request's success response
   * @return false if the request was superseded or closed, or the events
   *         are gone; true from a worker may still lose to an earlier final
   *         taken in
   */
  template <typename TResponse>
  bool respond(TResponse&& value) const
  {
    return sendRole<detail::ResponseRole::Success>(
        std::forward<TResponse>(value));
  }

  /**
   * Sends a progress response, with more to come (a long path's next leg),
   * if the request declares one (`progress = P`). Any number, until a final.
   * From a worker it's taken in at the next step, and dropped if a final was
   * taken in first.
   * @param value the request's progress response
   * @return false if the request was superseded or closed, or the events
   *         are gone
   */
  template <typename TResponse>
  bool progress(TResponse&& value) const
  {
    return sendRole<detail::ResponseRole::Progress>(
        std::forward<TResponse>(value));
  }

  /**
   * Sends the request's failure response, its final answer, if it declares
   * one (`failure = F`): the request closes once it's taken in, as
   * respond()'s does, and the first final taken in wins.
   * @param value the request's failure response
   * @return false if the request was superseded or closed, or the events
   *         are gone; true from a worker may still lose to an earlier final
   *         taken in
   */
  template <typename TResponse>
  bool fail(TResponse&& value) const
  {
    return sendRole<detail::ResponseRole::Failure>(
        std::forward<TResponse>(value));
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

  // A response in a role the request declares (checked as it compiles):
  // the success and the failure are final.
  template <detail::ResponseRole kRole, typename TResponse>
  bool sendRole(TResponse&& value) const
  {
    using Value = std::remove_cvref_t<TResponse>;
    static_assert(detail::checkRoleSent<TRequest, kRole, Value>());
    if constexpr (std::is_same_v<detail::RoleType<TRequest, kRole>, Value>)
      return deliver(
          detail::ResponseTo<TRequest, Value>{std::forward<TResponse>(value)},
          kRole != detail::ResponseRole::Progress);
    else
      return false;
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
