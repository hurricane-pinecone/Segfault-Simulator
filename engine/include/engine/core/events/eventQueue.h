#pragma once

#include "engine/core/ecs/entityDeclaration.h"
#include "engine/core/events/eventInbox.h"
#include "engine/core/events/eventOwner.h"
#include "engine/core/events/eventRules.h"
#include "engine/core/events/eventTraits.h"
#include "engine/core/events/keyed.h"
#include "engine/core/events/requestTickets.h"
#include "engine/core/util/typeKey.h"
#include "engine/core/util/typeList.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>
#if !defined(_WIN32)
  #include <pthread.h>
#endif

// One event type's queue: what Events keeps per type, and what its
// consumers and broadcasts point at. See events.h for how events are sent,
// consumed and kept.
namespace sfs
{

/** One event type's counts, for the debug view (Events::stats()). */
struct EventStats
{
  /**
   * The event type's name, for people ("EnemyDied", "sfs::nav::AgentEvent");
   * valid while its Events lives.
   */
  std::string_view type;
  /**
   * Its kind in words ("news", "entity news", "request", "response");
   * valid while its Events lives.
   */
  std::string_view kind;
  /**
   * Events sent in the last whole frame: the main thread's, and the workers'
   * taken in as it began.
   */
  std::size_t sentLastFrame = 0;
  /** Events kept now: those some consumer hasn't read yet. */
  std::size_t kept = 0;
  /** Workers' events waiting for the next frame. */
  std::size_t staged = 0;
  /** Room in the queue's buffer, in events: the most it has kept. */
  std::size_t capacity = 0;
  /**
   * The memory a request type's outstanding requests hold (their tickets),
   * in bytes; 0 for other types.
   */
  std::size_t requestBytes = 0;
  /** Consumers registered now: the EventConsumers made of the type. */
  uint32_t consumers = 0;
  /**
   * Events dropped at the kept cap before a consumer read them, over every
   * consumer, in all.
   */
  uint64_t missed = 0;
  /**
   * Keyed types: current events superseded by a newer send, a cancel or
   * their entity's destruction, in all.
   */
  uint64_t superseded = 0;
  /** Response types: responses dropped because their request was superseded. */
  uint64_t dropped = 0;
};

/**
 * One consumer of one event type, for the debug view
 * (Events::consumerStats()).
 */
struct EventConsumerStats
{
  /** The event type's name; valid while its Events lives. */
  std::string_view type;
  /** The consumer: its owner's name, or "a consumer" for one with none. */
  std::string_view consumer;
  /** How it reads: "read", "readLatest", or "" before its first read. */
  std::string_view how;
  /** Events kept that it hasn't read. */
  std::size_t unconsumed = 0;
};

namespace detail
{

#if defined(_WIN32)
/** The calling thread's platform id (GetCurrentThreadId()). */
std::uintptr_t currentThreadId();
#else
/**
 * A pthread_t as a number: an integer on Linux, a pointer elsewhere (only the
 * branch that applies is compiled, as this is a template).
 * @param thread the thread
 * @return its id
 */
template <typename TThread>
std::uintptr_t threadIdOf(TThread thread)
{
  if constexpr (std::is_pointer_v<TThread>)
    return reinterpret_cast<std::uintptr_t>(thread);
  else
    return static_cast<std::uintptr_t>(thread);
}

/**
 * The calling thread's platform id (pthread_self()), inline: every send
 * compares it with its owner's.
 */
inline std::uintptr_t currentThreadId() { return threadIdOf(pthread_self()); }
#endif

/**
 * Logs a worker's send, naming the thread (debugFeatures::logEvents()).
 * @param type the event type's name, for people
 */
void logWorkerSend(std::string_view type);

/**
 * A run of a queue's events, with the values kept beside them (null where the
 * type has none): what event ranges go through.
 */
template <typename T>
struct EventSegment
{
  /** The events. */
  const T* events = nullptr;
  /** Each event's key entity (keyed types). */
  const Entity* keys = nullptr;
  /** Each event's generation (keyed types). */
  const uint32_t* stamps = nullptr;
  /**
   * Each request's or response's ticket: its requester's request about its
   * key (RequestTickets).
   */
  const uint32_t* tickets = nullptr;
  /** How many events. */
  std::size_t count = 0;
};

/** Which of a queue's events a consumer reads. */
enum class Consuming : uint8_t
{
  /** None read yet: it holds every event, as All does. */
  Unknown,
  /** Every event: read(). */
  All,
  /**
   * Only events still current for their key (readLatest(), responses): it
   * doesn't hold superseded ones.
   */
  Latest,
};

/**
 * One consumer of a queue: an EventConsumer, or a requester's consumer of an
 * response type. The queue keeps an event until every consumer has passed it.
 * Its span lives here, beside its place, so its handle is just a slot.
 */
struct ConsumerSlot
{
  /** The number of the first event it hasn't read. */
  uint64_t next = 0;
  /**
   * A latest consumer's place in the frame boundary's scan: the events
   * before it are superseded, which they stay, so the next scan starts
   * there.
   */
  uint64_t scanned = 0;
  /** Its owner's current span: the first event, and past the last. */
  uint64_t spanBegin = 0;
  uint64_t spanEnd = 0;
  /** The owner's update stamp the span is for. */
  uint32_t spanStamp = 0;
  /**
   * Goes up each time the slot is given to a consumer, so an owner's
   * reference to a consumer gone since is told apart from the slot's next.
   */
  uint32_t generation = 0;
  /**
   * Its owner (a system, an object); null for a consumer made with none,
   * whose every read is a span of its own.
   */
  EventOwner* owner = nullptr;
  /** Its name, for the log and the debug view: its owner's, or "a consumer". */
  std::string name;
  /** Which events it reads. */
  Consuming consuming = Consuming::Unknown;
  /** Whether the slot is in use. */
  bool live = false;
  /** Whether a miss has been logged for it. */
  bool missedLogged = false;
};

/**
 * Where a queue is, shared with its consumers, so a consumer that outlives
 * its queue finds it gone rather than freed.
 */
struct EventQueueLink
{
  /** The queue; null once it's gone. */
  EventQueueBase* queue = nullptr;
};

/**
 * What an event queue does that doesn't depend on its type, so Events can
 * keep the queues of every type in one list.
 */
class EventQueueBase
{
public:
  /**
   * @param name the event type's name, for people
   * @param frame the owning Events' frame count, for the log
   * @param maxKept the owning Events' cap on kept events per type
   * @param kind the type's kind in words (eventKindName())
   */
  EventQueueBase(std::string name,
                 const uint64_t& frame,
                 const std::size_t& maxKept,
                 std::string kind);
  EventQueueBase(const EventQueueBase&) = delete;
  virtual ~EventQueueBase();
  EventQueueBase& operator=(const EventQueueBase&) = delete;
  /**
   * Registers a consumer, at the oldest event kept, so nothing sent while a
   * scene sets up is lost to the order things are made in.
   * @param owner its owner; null for none
   * @param who its owner's type, for the log; null for none
   * @return its slot
   */
  uint32_t addConsumer(EventOwner* owner, const std::type_info* who);
  /**
   * The frame boundary: the workers' staged events are taken in, the events
   * every consumer has passed are dropped (a type with no consumers keeps an
   * event until the second boundary after it was sent), and the buffer is
   * compacted. Events::update() calls it only while busy().
   */
  virtual void advance() = 0;
  /**
   * A range of this queue's events starts being gone through: until the last
   * ends, sends wait in a pending lane, so the buffer the range points into
   * never moves. Inline: every range calls it.
   */
  inline void beginRead() { ++m_reading; }
  /**
   * Whether advance() has work: events kept, waiting behind a range, or
   * workers' events staged. Not virtual, so an idle queue costs
   * Events::update() a load or two.
   */
  inline bool busy() const
  {
    return m_endId != m_oldestId || m_pendingCount != 0 ||
           (m_staged && m_staged->load(std::memory_order_relaxed) != 0);
  }
  /**
   * A requester goes, or never handled this queue's responses: its route
   * is closed (response queues only).
   * @param requester its number
   */
  virtual void closeRoute(uint32_t requester) {}
  /**
   * A registered consumer.
   * @param slot its slot (addConsumer())
   * @return it
   */
  inline ConsumerSlot& consumer(uint32_t slot) { return m_consumers[slot]; }
  /** @copydoc consumer(uint32_t) */
  inline const ConsumerSlot& consumer(uint32_t slot) const
  {
    return m_consumers[slot];
  }
  /**
   * This queue's consumers, for the debug view.
   * @param out gains one row per registered consumer
   */
  void consumerStats(std::vector<EventConsumerStats>& out) const;
  /**
   * The number the next event sent will get.
   * @return it
   */
  inline uint64_t endId() const { return m_endId; }
  /**
   * A range ends: once none is left, the events sent meanwhile join the
   * buffer, numbered after the rest in send order.
   */
  inline void endRead()
  {
    if (--m_reading == 0 && m_pendingCount != 0) [[unlikely]]
      foldPending();
  }
  /**
   * Whether any consumer is registered.
   * @return whether one is
   */
  inline bool hasConsumers() const { return m_liveConsumers != 0; }

  /**
   * Whether a consumer has events it hasn't read: events past its place.
   * @param slot its slot
   * @return whether it has
   */
  inline bool hasNew(uint32_t slot) const
  {
    const ConsumerSlot& self = m_consumers[slot];
    return self.next < m_endId;
  }

  /** The shared block consumers hold to find the queue. */
  inline const std::shared_ptr<EventQueueLink>& link() const { return m_link; }
  /**
   * Logs a cancel of a key's current event.
   * @param key the key entity
   * @param generation the cancel's generation
   */
  void logCancelled(const Entity& key, uint32_t generation) const;
  /**
   * Logs a response dropped because its request was superseded.
   * @param key the request's key entity
   * @param generation the request's generation
   */
  void logDropped(const Entity& key, uint32_t generation) const;
  /** Whether each send is logged (debugFeatures::logEvents()). */
  inline bool logging() const { return m_log; }

  /**
   * Logs a send with its number and frame (debugFeatures::logEvents()).
   * @param id the event's number
   */
  void logSent(uint64_t id) const;
  /**
   * Logs a keyed send with its number, key and generation.
   * @param id the event's number
   * @param key its key entity
   * @param generation its generation
   */
  void logSent(uint64_t id, const Entity& key, uint32_t generation) const;
  /**
   * Logs workers' events just taken in.
   * @param first the first one's number
   * @param count how many there were
   */
  void logTaken(uint64_t first, std::size_t count) const;
  /**
   * Counts events dropped at the cap before a consumer read them, and warns
   * once for that consumer, in every build.
   * @param count how many it missed
   * @param consumer the consumer
   * @param disabled whether its owner was disabled since its last update
   */
  void missed(uint64_t count, ConsumerSlot& consumer, bool disabled = false);
  /** The event type's name, for people. */
  inline const std::string& name() const { return m_name; }
  /**
   * The number of the oldest event kept.
   * @return it
   */
  inline uint64_t oldestId() const { return m_oldestId; }
  /**
   * Deregisters a consumer: the queue no longer keeps events for it.
   * @param slot its slot (addConsumer())
   */
  void removeConsumer(uint32_t slot);
  /**
   * A consumer's span for this call, with the way it reads noted: its
   * owner's update keeps one span for all its reads and handlers; a read
   * with no owner, or outside an update, is a span of its own.
   * @param slot its slot
   * @param consuming how it reads now
   * @param begin set to the span's first event
   * @param end set to past its last
   */
  inline void
  span(uint32_t slot, Consuming consuming, uint64_t& begin, uint64_t& end)
  {
    ConsumerSlot& self = m_consumers[slot];
    const EventOwner* owner = self.owner;

    if (!owner || self.spanStamp != owner->stamp || !owner->updating)
        [[unlikely]]
      newSpan(self);

    if (self.consuming != consuming) [[unlikely]]
      self.consuming =
          self.consuming == Consuming::Unknown ? consuming : Consuming::All;

    begin = self.spanBegin;
    end = self.spanEnd;
    m_lastReader = slot;
  }
  /**
   * This queue's counts, for the debug view.
   * @param out filled with them
   */
  virtual void stats(EventStats& out) const = 0;
  /**
   * Takes in what workers staged now, between frame boundaries: response
   * types' queues do (Events::takeIn()), others have nothing to do.
   */
  inline virtual void takeIn() {}

  /** Kept events past which a queue warns, naming who holds them. */
  static constexpr std::size_t kManyEvents = 65536;

protected:
  // Moves the events sent while ranges were alive into the buffer.
  virtual void foldPending() = 0;
  // A range of this queue lives past a frame boundary: warned once per
  // queue (in every build), and past 65,536 waiting events again.
  void keptRange();
  // A new span for a consumer: from its place (a miss at the cap is logged)
  // to the newest event.
  void newSpan(ConsumerSlot& self);
  // Events past the kept cap: logged as an error, once per queue, naming the
  // consumer that holds the oldest.
  void overCap(uint64_t dropped, const ConsumerSlot* holder);
  // The pending lane past the kept cap: its oldest were dropped, logged once
  // and counted as missed.
  void pendingOverCap(std::size_t dropped);
  // A requester's route of unread responses grew past 65,536: its owner
  // doesn't run (disabled, gone). Warned once per route.
  void routeGrows(std::string_view requester, std::size_t count);
  // Warns, once per queue, that many events are kept, naming the consumer
  // that holds the oldest.
  void warnMany(std::size_t kept, const ConsumerSlot* holder);

  // The oldest event kept, and the next to be sent.
  uint64_t m_oldestId = 0;
  uint64_t m_endId = 0;
  // The first event sent since the last frame boundary, and how many were
  // sent in the frame before it.
  uint64_t m_frameStartId = 0;
  std::size_t m_sentLastFrame = 0;
  // The inbox's staged count, once there is an inbox.
  const std::atomic<uint32_t>* m_staged = nullptr;
  std::vector<ConsumerSlot> m_consumers;
  uint32_t m_liveConsumers = 0;
  std::shared_ptr<EventQueueLink> m_link;
  std::string m_name;
  std::string m_kind;
  // Whether each send is logged (debugFeatures::logEvents()).
  bool m_log = false;
  // The frame advance() last ran in, which its sent count is for.
  uint64_t m_advancedFrame = 0;
  const uint64_t& m_frame;
  const std::size_t& m_maxKept;
  uint64_t m_missed = 0;
  uint64_t m_dropped = 0;
  bool m_warnedMany = false;
  bool m_warnedCap = false;
  // How many ranges of this queue are alive, and how many events wait in
  // the pending lane meanwhile.
  uint32_t m_reading = 0;
  uint32_t m_pendingCount = 0;
  bool m_warnedKeptRange = false;
  // The consumer whose span was taken last: likely the one keeping a range.
  uint32_t m_lastReader = 0;
  bool m_warnedPendingMany = false;
};

/**
 * The events of one type, numbered in send order from 0, in one contiguous
 * buffer: the buffer holds the events numbered from its base on, and those
 * from the oldest kept (the head) on are live. Beside the events, as arrays
 * of their own, are each event's key entity and generation (keyed types). At
 * each frame boundary the head moves to the slowest consumer; the buffer is
 * cleared when every event is read (the common case) and otherwise its read
 * part is erased once the head passes half of it, so no event is moved more
 * than about once and nothing is allocated once warm.
 */
template <typename T>
class EventQueue final : public EventQueueBase
{
  // Checked here, where every path (send, consumers, broadcast) makes the
  // queue.
  static_assert(std::is_object_v<T> && !std::is_pointer_v<T> &&
                    !std::is_const_v<T> && !std::is_volatile_v<T>,
                "An event is a value: a struct, not a pointer, reference or "
                "const type");
  static_assert(std::is_nothrow_move_constructible_v<T>,
                "An event type must be nothrow-movable");
  static_assert(checkEvent<T>());

  template <typename>
  friend class EventQueue;

  // T's event bases, every level (eventTraits.h), and how many.
  using Ancestors = typename EventTraits<T>::Ancestors;
  static constexpr std::size_t kAncestors = kListSize<Ancestors>;

  // Whether each event carries its request's ticket (requests and
  // responses).
  static constexpr bool kRequesters = kRequest<T> || ResponseQueueType<T>;

  // The events and the values beside them.
  struct Buffer
  {
    std::vector<T> events;
    [[no_unique_address]] Lane<kKeyed<T>, Entity> keys;
    [[no_unique_address]] Lane<kKeyed<T>, uint32_t> stamps;
    [[no_unique_address]] Lane<kRequesters, uint32_t> tickets;
  };

public:
  /**
   * A queue of a type that isn't a response; a keyed type's makes its
   * channel.
   * @param frame the owning Events' frame count, for the log
   * @param maxKept the owning Events' cap on kept events per type
   */
  EventQueue(const uint64_t& frame, const std::size_t& maxKept)
    requires(!ResponseQueueType<T>)
      : EventQueueBase(readableTypeName(componentTypeKey<T>()),
                       frame,
                       maxKept,
                       eventKindName(kKind<T>))
  {
    if constexpr (kOwnChannel<T>)
    {
      m_channel = std::make_shared<KeyedChannel>();
      m_channel->name = m_name;

      if constexpr (kRequest<T>)
        m_channel->tickets = std::make_unique<RequestTickets>();
    }
  }

  /**
   * The queue of a request type's responses of one type, which takes its inbox
   * from the request type's channel.
   * @param frame the owning Events' frame count, for the log
   * @param maxKept the owning Events' cap on kept events per type
   * @param type its id (eventTypeId() of the ResponseTo<R, A>)
   * @param requests the request type's channel
   * @param watchers the response type's own queue, which its consumers watch
   */
  EventQueue(const uint64_t& frame,
             const std::size_t& maxKept,
             int type,
             std::shared_ptr<KeyedChannel> requests,
             EventQueueBase& watchers)
    requires ResponseQueueType<T>
      : EventQueueBase(
            readableTypeName(componentTypeKey<typename T::Value>()) +
                " (response to " +
                readableTypeName(componentTypeKey<typename T::Request>()) + ")",
            frame,
            maxKept,
            eventKindName(EventKind::Response)),
        m_channel(std::move(requests)), m_watchers(&watchers)
  {
    {
      const std::lock_guard lock(m_channel->mutex);
      m_inbox = m_channel->template responseInbox<T>(type, nullptr);
    }

    if (!m_inbox)
      return;

    const std::lock_guard lock(m_inbox->mutex);
    m_inbox->log = m_log;
    m_inbox->name = m_name;
    m_staged = &m_inbox->count;
  }

  ~EventQueue() override
  {
    if (!m_inbox)
      return;

    // Staged events go now, while T's code is surely loaded.
    typename EventInbox<T>::Lanes staged;
    {
      const std::lock_guard lock(m_inbox->mutex);
      m_inbox->open = false;
      std::swap(staged, m_inbox->staged);
      m_inbox->count.store(0, std::memory_order_relaxed);
    }
  }

  /**
   * The events numbered [from, to), kept to those held now, with the values
   * beside them.
   * @param from the first event's number (one before oldestId() gives from
   *        there)
   * @param to past the last one's (one past endId() gives to there)
   * @return them
   */
  EventSegment<T> segment(uint64_t from, uint64_t to)
  {
    if (from < m_oldestId)
      from = m_oldestId;

    if (to > m_endId)
      to = m_endId;

    if (to < from)
      to = from;

    const auto first = static_cast<std::size_t>(from - m_baseId);
    EventSegment<T> part;
    part.events = m_buffer.events.data() + first;

    if constexpr (kKeyed<T>)
    {
      part.keys = m_buffer.keys.data() + first;
      part.stamps = m_buffer.stamps.data() + first;
    }

    if constexpr (kRequesters)
      part.tickets = m_buffer.tickets.data() + first;

    part.count = static_cast<std::size_t>(to - from);
    return part;
  }

  /**
   * Appends an unkeyed event, and a copy of it as each event base to the
   * base's queue while it has consumers.
   * @param event the event, copied or moved in
   */
  template <typename TEvent>
  void push(TEvent&& event)
    requires(!kKeyed<T>)
  {
    const T& kept = append(std::forward<TEvent>(event));

    if constexpr (kAncestors != 0)
      fanOut(kept, static_cast<Ancestors*>(nullptr));
  }

  /**
   * Appends a keyed event, and a copy of it as each event base to the base's
   * queue while it has consumers (with the key's generation there).
   * @param event the event, copied or moved in
   * @param key its key entity
   * @param generation its generation: its key's, or a response's request's
   * @param ticket a request's or response's ticket (RequestTickets)
   * @return the event as kept
   */
  template <typename TEvent>
  const T& push(TEvent&& event,
                const Entity& key,
                uint32_t generation,
                uint32_t ticket = 0)
    requires kKeyed<T>
  {
    const T& kept =
        append(std::forward<TEvent>(event), key, generation, ticket);

    if constexpr (kAncestors != 0)
      fanOut(kept, static_cast<Ancestors*>(nullptr), &key);

    return kept;
  }

  /**
   * Links the queue of one of T's event bases (its ancestors,
   * eventTraits.h).
   * @param index its place in T's ancestor list
   * @param base its queue
   */
  void setAncestor(std::size_t index, EventQueueBase& base)
  {
    if constexpr (kAncestors != 0)
      m_ancestors[index] = &base;
  }

  /**
   * A main-thread response: kept while its request is current, else dropped.
   * @param response the response
   * @param key its request's key
   * @param generation its request's generation
   * @param last whether it is its request's final response
   * @param ticket the request's ticket
   * @return whether it was kept
   */
  template <typename TEvent>
  bool respond(TEvent&& response,
               const Entity& key,
               uint32_t generation,
               bool last,
               uint32_t ticket)
    requires ResponseQueueType<T>
  {
    RequestTickets& requests = *m_channel->tickets;

    // Superseded, or closed by its final response: the first final wins.
    if (!requests.current(ticket, generation) ||
        requests.closed(ticket, generation))
    {
      ++m_dropped;

      if (m_log) [[unlikely]]
        logDropped(key, generation);

      return false;
    }

    const T& kept =
        push(std::forward<TEvent>(response), key, generation, ticket);

    if (last)
      requests.responded(ticket, generation);

    route(requests.requesterOf(ticket), lastId());
    watch(kept, key, generation);
    return true;
  }

  /**
   * Opens a requester's route: its responses of this type are noted for it
   * as they're kept, from now on.
   * @param requester its number
   * @param name its owner's name, for the warning on a route that grows
   */
  void openRoute(uint32_t requester, std::string name)
    requires ResponseQueueType<T>
  {
    if (m_routes.size() < requester)
      m_routes.resize(requester);

    Route& own = m_routes[requester - 1];
    own.open = true;
    own.name = std::move(name);
  }

  void closeRoute(uint32_t requester) override
  {
    if constexpr (ResponseQueueType<T>)
      if (requester != 0 && requester <= m_routes.size())
      {
        // `reading` may still be gone through by a live range (a requester
        // destroyed in its own handler); takeRoute() empties it next time.
        Route& gone = m_routes[requester - 1];
        gone.open = false;
        gone.warned = false;
        gone.ids.clear();
      }
  }

  /**
   * A requester's responses up to its span's end, oldest first, for it to go
   * through: moved off its route, so each is read once. Valid until its next
   * call for the requester.
   * @param requester its number
   * @param end the span's end
   * @return the ids
   */
  const std::vector<uint64_t>& takeRoute(uint32_t requester, uint64_t end)
    requires ResponseQueueType<T>
  {
    static const std::vector<uint64_t> none;

    if (requester == 0 || requester > m_routes.size())
      return none;

    Route& own = m_routes[requester - 1];
    own.reading.clear();
    std::size_t taken = 0;

    while (taken < own.ids.size() && own.ids[taken] < end)
      ++taken;

    own.reading.insert(own.reading.end(),
                       own.ids.begin(),
                       own.ids.begin() + static_cast<std::ptrdiff_t>(taken));
    own.ids.erase(
        own.ids.begin(), own.ids.begin() + static_cast<std::ptrdiff_t>(taken));
    return own.reading;
  }

  /**
   * The channel of entity news or a request (its own), or of a response
   * queue (its request type's); null for a response type's own queue.
   * @return it
   */
  const std::shared_ptr<KeyedChannel>& channel() const
    requires kKeyed<T>
  {
    return m_channel;
  }

  /**
   * The inbox workers' broadcasts share, made the first time.
   * @return it
   */
  const std::shared_ptr<EventInbox<T>>& inbox()
    requires(!ResponseQueueType<T>)
  {
    if (!m_inbox)
    {
      m_inbox = std::make_shared<EventInbox<T>>();
      m_inbox->log = m_log;
      m_inbox->name = m_name;
      m_staged = &m_inbox->count;
    }

    return m_inbox;
  }

  void advance() override
  {
    // A range kept across the frame boundary points into the buffer: it
    // stays as it is until the range ends, and what's sent waits, capped.
    if (m_reading != 0) [[unlikely]]
    {
      heldBack();
      return;
    }

    const uint64_t lastStart = m_frameStartId;
    m_sentLastFrame = static_cast<std::size_t>(m_endId - lastStart);
    m_advancedFrame = m_frame;
    m_frameStartId = m_endId;

    if (m_staged && m_staged->load(std::memory_order_relaxed) != 0)
      takeStaged();

    const ConsumerSlot* holder = nullptr;
    uint64_t head = hasConsumers() ? slowest(holder) : lastStart;

    if (head < m_oldestId)
      head = m_oldestId;

    const uint64_t kept = m_endId - head;
    const bool capped = kept > m_maxKept;

    if (capped) [[unlikely]]
    {
      overCap(kept - m_maxKept, holder);
      head = m_endId - m_maxKept;
      pruneRoutes(head);
    }
    else if (kept > kManyEvents && !m_warnedMany) [[unlikely]]
      warnMany(static_cast<std::size_t>(kept), holder);

    // Latest consumers behind the head had nothing left there to read (only
    // superseded events), unless the cap moved it: then the current events
    // they could still have read are counted as missed. They move up to it.
    for (ConsumerSlot& consumer : m_consumers)
      if (consumer.live && consumer.next < head &&
          consumer.consuming == Consuming::Latest)
      {
        if (capped) [[unlikely]]
          countCapMisses(consumer, head);

        consumer.next = head;
      }

    m_oldestId = head;
    compact();
  }

  void takeIn() override
  {
    if (m_staged && m_staged->load(std::memory_order_relaxed) != 0)
      takeStaged();
  }

  void stats(EventStats& out) const override
  {
    out.type = m_name;
    out.kind = m_kind;
    // An idle queue's frame boundary skips advance(): it sent nothing.
    out.sentLastFrame = m_advancedFrame == m_frame ? m_sentLastFrame : 0u;
    out.kept = static_cast<std::size_t>(m_endId - m_oldestId) + m_pendingCount;
    out.staged = m_inbox ? m_inbox->count.load(std::memory_order_relaxed) : 0u;
    out.capacity = m_buffer.events.capacity();
    out.consumers = m_liveConsumers;
    out.missed = m_missed;
    out.dropped = m_dropped;

    if constexpr (kRequest<T>)
    {
      out.superseded = m_channel->superseded + m_channel->tickets->superseded;
      out.requestBytes = m_channel->tickets->bytes();
    }
    else if constexpr (kOwnChannel<T>)
      out.superseded = m_channel->superseded;
  }

private:
  // The oldest event a consumer still holds: the slowest consumer's place,
  // where a Latest one skips superseded events. `holder` is set to the
  // consumer holding it.
  uint64_t slowest(const ConsumerSlot*& holder)
  {
    uint64_t head = m_endId;

    for (ConsumerSlot& consumer : m_consumers)
    {
      if (!consumer.live)
        continue;

      uint64_t at = consumer.next < m_oldestId ? m_oldestId : consumer.next;

      if constexpr (kOwnChannel<T>)
        if (consumer.consuming == Consuming::Latest)
        {
          if (consumer.scanned > at)
            at = consumer.scanned;

          while (at < m_endId && !current(at))
            ++at;

          consumer.scanned = at;
        }

      if (at < head || !holder)
      {
        head = at < head ? at : head;
        holder = &consumer;
      }
    }

    return head;
  }

  // A latest consumer's current events the cap is dropping, counted as
  // missed.
  void countCapMisses(ConsumerSlot& consumer, uint64_t head)
  {
    uint64_t missedEvents = 0;

    if constexpr (kOwnChannel<T>)
      for (uint64_t id = consumer.next < m_oldestId ? m_oldestId
                                                    : consumer.next;
           id < head;
           ++id)
        missedEvents += current(id);

    if (missedEvents != 0)
      missed(missedEvents, consumer);
  }

  // Whether the event numbered `id` is still its key's current one.
  bool current(uint64_t id) const
    requires kKeyed<T>
  {
    const auto at = static_cast<std::size_t>(id - m_baseId);

    if constexpr (kRequesters)
      return m_channel->tickets->current(
          m_buffer.tickets[at], m_buffer.stamps[at]);
    else
      return m_channel->current(m_buffer.keys[at].getId(), m_buffer.stamps[at]);
  }

  // Drops the events before the head from the buffer: all of it when every
  // event is read, else the read part once it's half the buffer.
  void compact()
  {
    const auto consumed = static_cast<std::size_t>(m_oldestId - m_baseId);

    if (consumed == 0)
      return;

    if (m_oldestId == m_endId)
    {
      m_buffer.events.clear();

      if constexpr (kKeyed<T>)
      {
        m_buffer.keys.clear();
        m_buffer.stamps.clear();

        if constexpr (kRequesters)
          m_buffer.tickets.clear();
      }

      m_baseId = m_oldestId;
      return;
    }

    if (consumed * 2 < m_buffer.events.size())
      return;

    eraseFront(m_buffer.events, consumed);

    if constexpr (kKeyed<T>)
    {
      eraseFront(m_buffer.keys, consumed);
      eraseFront(m_buffer.stamps, consumed);

      if constexpr (kRequesters)
        eraseFront(m_buffer.tickets, consumed);
    }

    m_baseId = m_oldestId;
  }

  // Erases a lane's first `count` values, keeping its room. A type that
  // can't be assigned is moved into the spare buffer instead, which is then
  // swapped in (its room is kept too).
  template <typename TValue>
  void eraseFront(std::vector<TValue>& lane, std::size_t count)
  {
    if constexpr (std::is_move_assignable_v<TValue>)
      lane.erase(
          lane.begin(), lane.begin() + static_cast<std::ptrdiff_t>(count));
    else
    {
      m_spare.clear();
      m_spare.reserve(lane.size() - count);

      for (std::size_t i = count; i < lane.size(); ++i)
        m_spare.emplace_back(std::move(lane[i]));

      lane.swap(m_spare);
    }
  }

  // Appends an unkeyed event, alone: to the pending lane while a range is
  // alive. Gives the event as kept.
  template <typename TEvent>
  const T& append(TEvent&& event)
    requires(!kKeyed<T>)
  {
    if (m_reading != 0) [[unlikely]]
    {
      m_pending.events.push_back(std::forward<TEvent>(event));
      ++m_pendingCount;

      if (m_log) [[unlikely]]
        logSent(m_endId + m_pendingCount - 1);

      return m_pending.events.back();
    }

    m_buffer.events.push_back(std::forward<TEvent>(event));
    ++m_endId;

    if (m_log) [[unlikely]]
      logSent(m_endId - 1);

    return m_buffer.events.back();
  }

  // Appends a keyed event, alone, as append(event) does.
  template <typename TEvent>
  const T& append(TEvent&& event,
                  const Entity& key,
                  uint32_t generation,
                  uint32_t ticket = 0)
    requires kKeyed<T>
  {
    Buffer& lane = m_reading != 0 ? m_pending : m_buffer;
    lane.events.push_back(std::forward<TEvent>(event));
    lane.keys.push_back(key);
    lane.stamps.push_back(generation);

    if constexpr (kRequesters)
      lane.tickets.push_back(ticket);

    if (m_reading != 0) [[unlikely]]
      ++m_pendingCount;
    else
      ++m_endId;

    if (m_log) [[unlikely]]
      logSent(m_endId + m_pendingCount - 1, key, generation);

    return lane.events.back();
  }

  void foldPending() override
  {
    if (m_reading != 0)
      return;

    if constexpr (std::is_move_assignable_v<T>)
      m_buffer.events.insert(m_buffer.events.end(),
                             std::make_move_iterator(m_pending.events.begin()),
                             std::make_move_iterator(m_pending.events.end()));
    else
      for (T& event : m_pending.events)
        m_buffer.events.emplace_back(std::move(event));

    if constexpr (kKeyed<T>)
    {
      m_buffer.keys.insert(
          m_buffer.keys.end(), m_pending.keys.begin(), m_pending.keys.end());
      m_buffer.stamps.insert(m_buffer.stamps.end(),
                             m_pending.stamps.begin(),
                             m_pending.stamps.end());
      m_pending.keys.clear();
      m_pending.stamps.clear();
    }

    if constexpr (kRequesters)
    {
      m_buffer.tickets.insert(m_buffer.tickets.end(),
                              m_pending.tickets.begin(),
                              m_pending.tickets.end());
      m_pending.tickets.clear();
    }

    m_pending.events.clear();
    m_endId += m_pendingCount;
    m_pendingCount = 0;
  }

  // A copy of `event` as each of its event bases, to each base's queue that
  // has consumers: an unused base costs a load. A keyed copy starts the
  // key's next generation in the base's own channel.
  template <typename... TBases>
  void fanOut(const T& event, TypeList<TBases...>*, const Entity* key = nullptr)
  {
    std::size_t index = 0;
    (
        [&]
        {
          auto& base = *static_cast<EventQueue<TBases>*>(m_ancestors[index++]);

          if (!base.hasConsumers())
            return;

          if constexpr (kKeyed<T>)
          {
            uint32_t generation = 0;

            if (base.channel()->next(*key, true, generation))
              base.append(static_cast<const TBases&>(event), *key, generation);
          }
          else
            base.append(static_cast<const TBases&>(event));
        }(),
        ...);
  }

  // The number the last event appended has, or will have once the pending
  // lane joins the buffer.
  uint64_t lastId() const
  {
    return m_reading != 0 ? m_endId + m_pendingCount - 1 : m_endId - 1;
  }

  // Notes a response kept for its requester, if that requester handles this
  // type (its route is open): a requester's span goes through its own.
  void route(uint32_t requester, uint64_t id)
    requires ResponseQueueType<T>
  {
    if (requester == 0 || requester > m_routes.size() ||
        !m_routes[requester - 1].open)
      return;

    Route& own = m_routes[requester - 1];
    own.ids.push_back(id);

    if (own.ids.size() >= kManyEvents && !own.warned) [[unlikely]]
    {
      own.warned = true;
      routeGrows(own.name, own.ids.size());
    }
  }

  // The cap moved the head past `oldest`: routed ids below it name dropped
  // responses.
  void pruneRoutes(uint64_t oldest)
  {
    if constexpr (ResponseQueueType<T>)
      for (Route& own : m_routes)
      {
        std::size_t gone = 0;

        while (gone < own.ids.size() && own.ids[gone] < oldest)
          ++gone;

        own.ids.erase(own.ids.begin(),
                      own.ids.begin() + static_cast<std::ptrdiff_t>(gone));
      }
  }

  // The pending lane's oldest `drop` went: routed ids of those go, and the
  // later ones move down to the numbers they'll have.
  void shiftRoutes(std::size_t drop)
  {
    if constexpr (ResponseQueueType<T>)
    {
      const uint64_t first = m_endId;
      const uint64_t past = m_endId + drop;

      for (Route& own : m_routes)
      {
        std::size_t kept = 0;

        for (const uint64_t id : own.ids)
        {
          if (id >= first && id < past)
            continue;

          own.ids[kept++] = id >= past ? id - drop : id;
        }

        own.ids.resize(kept);
      }
    }
  }

  // A response kept: a copy goes to the response type's own queue while it
  // has consumers watching (copyable response types only), keyed by the
  // request's entity.
  void watch(const T& response, const Entity& key, uint32_t generation)
    requires ResponseQueueType<T>
  {
    using Value = typename T::Value;

    if constexpr (std::is_copy_constructible_v<Value>)
    {
      auto& watchers = *static_cast<EventQueue<Value>*>(m_watchers);

      if (watchers.hasConsumers()) [[unlikely]]
        watchers.append(static_cast<const Value&>(response), key, generation);
    }
  }

  // A frame boundary while a range is alive: warned once; the pending lane
  // is warned about and capped like the buffer.
  void heldBack()
  {
    // The workers' events wait behind the range too, under the same cap.
    if (m_staged && m_staged->load(std::memory_order_relaxed) != 0)
      takeStaged();

    keptRange();
    // Capped with the buffer: together they keep at most the cap.
    const uint64_t buffered = m_endId - m_oldestId;
    const uint64_t room = buffered < m_maxKept ? m_maxKept - buffered : 0;

    if (m_pendingCount <= room)
      return;

    const auto drop = static_cast<std::size_t>(m_pendingCount - room);
    eraseFront(m_pending.events, drop);

    if constexpr (kKeyed<T>)
    {
      eraseFront(m_pending.keys, drop);
      eraseFront(m_pending.stamps, drop);
    }

    if constexpr (kRequesters)
      eraseFront(m_pending.tickets, drop);

    shiftRoutes(drop);
    m_pendingCount -= static_cast<uint32_t>(drop);
    pendingOverCap(drop);
  }

  // Moves the workers' events in after those sent so far. The lock covers
  // only the swap of the lanes. Responses whose request was superseded are
  // dropped.
  void takeStaged()
  {
    typename EventInbox<T>::Lanes& spare = m_inbox->spare;
    {
      const std::lock_guard lock(m_inbox->mutex);
      std::swap(spare, m_inbox->staged);
      m_inbox->count.store(0, std::memory_order_relaxed);
    }
    // Behind a live range they wait in the pending lane, as sends do.
    const bool reading = m_reading != 0;
    Buffer& lane = reading ? m_pending : m_buffer;
    const std::size_t before = lane.events.size();
    const uint64_t first = m_endId + m_pendingCount;

    if constexpr (ResponseQueueType<T>)
    {
      RequestTickets& requests = *m_channel->tickets;

      for (std::size_t i = 0; i < spare.events.size(); ++i)
      {
        const uint32_t stamp = spare.stamps[i] & ~kLastResponse;
        const Entity& key = spare.keys[i];
        const uint32_t ticket = spare.tickets[i];

        if (!requests.current(ticket, stamp) || requests.closed(ticket, stamp))
        {
          ++m_dropped;

          if (m_log)
            logDropped(key, stamp);

          continue;
        }

        lane.events.emplace_back(std::move(spare.events[i]));
        lane.keys.push_back(key);
        lane.stamps.push_back(stamp);
        lane.tickets.push_back(ticket);

        if (spare.stamps[i] & kLastResponse)
          requests.responded(ticket, stamp);

        route(requests.requesterOf(ticket),
              first + (lane.events.size() - before) - 1);
        watch(lane.events.back(), key, stamp);
      }
    }
    else if constexpr (kOwnChannel<T>)
    {
      // A worker's keyed events get their generations here, in queue order;
      // one the channel refuses (a stale key, an id past its pages) is
      // dropped, as on the owning thread.
      for (std::size_t i = 0; i < spare.events.size(); ++i)
      {
        uint32_t generation = 0;

        if (!m_channel->next(spare.keys[i], true, generation))
          continue;

        lane.events.emplace_back(std::move(spare.events[i]));
        lane.keys.push_back(spare.keys[i]);
        lane.stamps.push_back(generation);

        if constexpr (kAncestors != 0)
          fanOut(lane.events.back(),
                 static_cast<Ancestors*>(nullptr),
                 &spare.keys[i]);
      }
    }
    else if constexpr (!kKeyed<T>)
    {
      // insert() copies trivial events as one block, but needs T to be
      // move-assignable; other types go one by one.
      if constexpr (std::is_move_assignable_v<T>)
        lane.events.insert(lane.events.end(),
                           std::make_move_iterator(spare.events.begin()),
                           std::make_move_iterator(spare.events.end()));
      else
      {
        lane.events.reserve(lane.events.size() + spare.events.size());

        for (T& event : spare.events)
          lane.events.emplace_back(std::move(event));
      }

      if constexpr (kAncestors != 0)
        for (std::size_t i = before; i < lane.events.size(); ++i)
          fanOut(lane.events[i], static_cast<Ancestors*>(nullptr));
    }

    const std::size_t added = lane.events.size() - before;

    if (reading)
      m_pendingCount += static_cast<uint32_t>(added);
    else
      m_endId = m_baseId + m_buffer.events.size();

    if (m_log && added != 0)
      logTaken(first, added);

    spare.clear();
  }

  Buffer m_buffer;
  // What's sent while a range is alive, joining the buffer as the last ends
  // (foldPending()).
  Buffer m_pending;
  // A response queue's routes, by requester number - 1: the ids of each
  // requester's responses it hasn't read, and those it's reading.
  struct Route
  {
    std::vector<uint64_t> ids;
    std::vector<uint64_t> reading;
    std::string name;
    bool open = false;
    bool warned = false;
  };
  [[no_unique_address]] std::
      conditional_t<ResponseQueueType<T>, std::vector<Route>, NoLane>
          m_routes;
  // The number of the buffer's first event.
  uint64_t m_baseId = 0;
  std::shared_ptr<EventInbox<T>> m_inbox;
  [[no_unique_address]] std::
      conditional_t<kKeyed<T>, std::shared_ptr<KeyedChannel>, NoLane>
          m_channel;
  // The response type's own queue, for a response queue: its consumers watch
  // every response kept.
  [[no_unique_address]] std::
      conditional_t<ResponseQueueType<T>, EventQueueBase*, NoLane>
          m_watchers;
  // The queues of T's event bases, in its ancestor list's order.
  [[no_unique_address]] std::conditional_t<
      kAncestors != 0,
      std::array<EventQueueBase*, kAncestors>,
      NoLane>
      m_ancestors{};
  // compact()'s scratch, for an event type that can't be assigned.
  [[no_unique_address]] std::
      conditional_t<std::is_move_assignable_v<T>, NoLane, std::vector<T>>
          m_spare;
};

} // namespace detail

} // namespace sfs
