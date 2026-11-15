#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/events/eventRules.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <vector>

// Where workers' events wait for the owning thread to take them in: an
// EventBroadcast's news and a Responder's responses (see events.h).
namespace sfs::detail
{

/** A lane a type's events don't have: takes no room. */
struct NoLane
{
};

/**
 * One value per event beside the events themselves (their keys or
 * generations), or nothing for a type that has none.
 */
template <bool kOn, typename TValue>
using Lane = std::conditional_t<kOn, std::vector<TValue>, NoLane>;

/**
 * What every inbox has, whatever its event type: its lock, its count and
 * whether it's still open. Shared by the queue and its broadcasts (or a
 * request type's channel and its responders), so a broadcast that outlives the
 * queue finds it closed rather than freed.
 */
struct EventInboxBase
{
  /** Guards the staged events and `open`. */
  std::mutex mutex;
  /** How many events are staged, read at a frame's start without the lock. */
  std::atomic<uint32_t> count{0};
  /** False once the queue is gone: sends then fail. */
  bool open = true;
  /** Whether each send is logged (debugFeatures::logEvents()). */
  bool log = false;
  /** The event type's name, for the log; valid while `open`. */
  std::string_view name;
};

/**
 * Generations staged with responses carry this bit when the response is its
 * request's final one (Responder::respond()).
 */
inline constexpr uint32_t kLastResponse = 1u << 31;

/** Where workers' events of one type wait for the next frame. */
template <typename T>
struct EventInbox : EventInboxBase
{
  /** Staged events, each with its key and generation where T has them. */
  struct Lanes
  {
    /** The events, in the order they were sent. */
    std::vector<T> events;
    /** Each event's key entity (keyed types). */
    [[no_unique_address]] Lane<kKeyed<T>, Entity> keys;
    /**
     * Each response's request generation, with kLastResponse set on its
     * request's final response (response types). Other keyed events get theirs
     * as they're taken in.
     */
    [[no_unique_address]] Lane<ResponseQueueType<T>, uint32_t> stamps;
    /** Each response's request ticket (response types). */
    [[no_unique_address]] Lane<ResponseQueueType<T>, uint32_t> tickets;

    /** Empties every lane, keeping its room. */
    void clear()
    {
      events.clear();
      if constexpr (kKeyed<T>)
        keys.clear();
      if constexpr (ResponseQueueType<T>)
      {
        stamps.clear();
        tickets.clear();
      }
    }
  };

  /** The workers' events. */
  Lanes staged;
  /**
   * The owning thread's: swapped with `staged` when they're taken in,
   * emptied into the queue, and handed back next time with its room kept.
   */
  Lanes spare;
};

} // namespace sfs::detail
