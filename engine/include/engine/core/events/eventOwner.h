#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

// What owns event handles: a system, a service or a game object.
namespace sfs::detail
{

class EventQueueBase;
struct EventQueueLink;

/** A consumer an owner made: its queue's link and its slot there. */
struct ConsumerRef
{
  /** Its queue's link (null once the queue is gone). */
  std::shared_ptr<EventQueueLink> link;
  /** Its slot on the queue. */
  uint32_t slot = 0;
  /** The slot's generation when the owner took it (EventOwner::add()). */
  uint32_t generation = 0;
};

/**
 * A handler one of the owner's consumers was given: run with the consumer's
 * span when it has something new.
 */
struct EventHandler
{
  /** The consumer's queue link. */
  std::shared_ptr<EventQueueLink> link;
  /** The consumer's slot. */
  uint32_t slot = 0;
  /** Runs the handler over the slot's span of the queue. */
  std::function<void(EventQueueBase&, uint32_t)> run;
  /** The slot's generation when it was given (EventOwner::addHandler()). */
  uint32_t generation = 0;
};

/**
 * What an owner of event handles keeps for them (a system, a service, a game
 * object): its update stamp, so each of its updates is one span for
 * all its consumers; whether it's in one; its consumers, which are what wakes
 * it; and their handlers, which its host runs in its slot (dispatch()). Its
 * handles point at it through their slots, so it outlives them: they're its
 * members.
 */
struct EventOwner
{
  EventOwner() = default;
  EventOwner(const EventOwner&) = delete;
  /**
   * Lets go of its consumers' slots, so a handle that outlives it (moved
   * into longer-lived storage) finds no owner to tell when it goes.
   */
  ~EventOwner();
  EventOwner& operator=(const EventOwner&) = delete;
  /**
   * Adds a consumer it made.
   * @param link the consumer's queue link
   * @param slot its slot
   */
  void add(const std::shared_ptr<EventQueueLink>& link, uint32_t slot);
  /**
   * Adds a handler, unless dispatch() is running (logged once, refused: it
   * would move the handler being run).
   * @param handler the handler
   */
  void addHandler(EventHandler handler);
  /**
   * Starts an update: a new span for every consumer, and `updating` set.
   */
  void begin();
  /**
   * Logs, once, a consuming call outside an update (builds without NDEBUG).
   * @param type the event type's name
   * @param who the consumer's name
   */
  void consumedOutside(std::string_view type, std::string_view who);
  /**
   * Runs each handler whose consumer has something new, in the order given,
   * with the update's span. Called by the host between begin() and the
   * owner's update.
   */
  void dispatch();
  /** Ends it: a call between updates gets a span of its own. */
  void end();
  /**
   * Notes that one of its consumers went, its slot freed already: from then
   * on its entry and handlers are skipped, and the gone ones are cleared out
   * once they're half of `consumers` (after dispatch(), if it's running), so
   * letting go of many consumers costs each the same.
   */
  void forget();
  /**
   * Whether any of its consumers' queues holds events past the consumer's
   * place: what a host can check before running an owner that only
   * consumes.
   * @return whether it has unconsumed events
   */
  bool hasUnconsumed() const;

  /** Goes up as each update begins and ends. */
  uint32_t stamp = 0;
  /** Whether an update (its handlers' dispatch included) is running. */
  bool updating = false;
  /** Whether the owner was disabled since its last update. */
  bool wasDisabled = false;
  /** Whether a consuming call outside an update was logged. */
  bool warnedOutside = false;
  /** Whether dispatch() is running its handlers. */
  bool dispatching = false;
  /** Whether a handler given during dispatch() was logged. */
  bool warnedLateHandler = false;
  /**
   * How many of its consumers went since `consumers` and `handlers` were
   * last cleared of them.
   */
  uint32_t forgotten = 0;
  /**
   * The consumers it made, in the order made: those gone since (their slot
   * freed or given to another) are skipped until they're cleared out.
   */
  std::vector<ConsumerRef> consumers;
  /** Their handlers, in the order given, kept as `consumers` is. */
  std::vector<EventHandler> handlers;
};

} // namespace sfs::detail
