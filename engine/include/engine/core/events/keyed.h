#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/events/eventInbox.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

// What a keyed event type keeps per entity: each key's generation, read
// by any thread, and the inboxes its responses come back through (see
// events.h, responder.h).
namespace sfs
{
class Events;
}

namespace sfs::detail
{

class RequestTickets;

/**
 * One word per entity id, in pages of 1,024 ids made the first time an id in
 * the page is written, under a fixed directory of 1,024 pages (1M ids, 8 KB).
 * Pages never move, so another thread may read a word whose page it knows is
 * made.
 */
template <typename TWord>
class PagedWords
{
public:
  /** Ids a page holds, as a power of two. */
  static constexpr uint32_t kPageBits = 10;
  /** Ids a page holds. */
  static constexpr uint32_t kPageSize = 1u << kPageBits;
  /** The highest id kept, plus one. */
  static constexpr uint32_t kIds = kPageSize * 1024;

  /**
   * The word for an id, if its page is made.
   * @param id the entity id
   * @return the word, or null
   */
  TWord* find(uint32_t id) const
  {
    if (id >= kIds) [[unlikely]]
      return nullptr;
    TWord* page = m_pages[id >> kPageBits].get();
    return page ? page + (id & (kPageSize - 1)) : nullptr;
  }

  /**
   * The word for an id, making its page (zeroed) the first time. Only the
   * thread that owns the words makes pages.
   * @param id the entity id
   * @return the word, or null for an id past kIds
   */
  TWord* make(uint32_t id)
  {
    if (id >= kIds) [[unlikely]]
      return nullptr;
    std::unique_ptr<TWord[]>& page = m_pages[id >> kPageBits];
    if (!page) [[unlikely]]
      page = std::make_unique<TWord[]>(kPageSize);
    return page.get() + (id & (kPageSize - 1));
  }

  /**
   * The memory it holds, in bytes: its directory and the pages made.
   * @return the bytes
   */
  std::size_t bytes() const
  {
    std::size_t total = sizeof(*this);
    for (const std::unique_ptr<TWord[]>& page : m_pages)
      if (page)
        total += kPageSize * sizeof(TWord);
    return total;
  }

private:
  std::array<std::unique_ptr<TWord[]>, kIds / kPageSize> m_pages;
};

/**
 * A key's word: its generation (which keyed send, request or cancel was its
 * last) shifted up by one, and a live bit, set while that last was a send.
 * @param generation the generation
 * @return its live word
 */
inline constexpr uint32_t liveWord(uint32_t generation)
{
  return (generation << 1) | 1u;
}

/** One response type's inbox in its request type's channel. */
struct ResponseSlot
{
  /** The response type's id (eventTypeId()). */
  int type = 0;
  /** Its inbox: an EventInbox of the response type. */
  std::shared_ptr<EventInboxBase> inbox;
  /**
   * Makes the response type's queue on the owning thread, for an inbox a
   * worker's response made first.
   */
  void (*adopt)(Events&) = nullptr;
  /** Whether the response type's queue has its inbox. */
  bool adopted = false;
};

/**
 * A keyed event type's shared block: each entity's generation, and for a
 * request the inboxes of its response types. Made with the type's queue and
 * shared with every Responder, so a responder that outlives the events finds
 * it closed rather than freed.
 */
class KeyedChannel : public std::enable_shared_from_this<KeyedChannel>
{
public:
  KeyedChannel();
  /** Frees the request tickets, if any. */
  ~KeyedChannel();
  KeyedChannel(const KeyedChannel&) = delete;
  KeyedChannel& operator=(const KeyedChannel&) = delete;

  /**
   * Whether a key handle is to an entity destroyed since it was taken: one
   * whose id has gone to a newer entity, or will. Owning thread only.
   * @param key the key
   * @return whether it is stale
   */
  inline bool stale(const Entity& key) const
  {
    const uint32_t* least =
        keyGenerations ? keyGenerations->find(key.getId()) : nullptr;
    return least && key.getGeneration() < *least;
  }

  /**
   * Starts a key's next generation (a keyed send or request, live; a cancel,
   * not live). Owning thread only.
   * @param key the key
   * @param live whether the new generation is a send's
   * @param generation set to the new generation
   * @return false for a stale key or an id past PagedWords::kIds (logged),
   *         changing nothing
   */
  inline bool next(const Entity& key, bool live, uint32_t& generation)
  {
    if (stale(key)) [[unlikely]]
      return refuseStale(key);
    const uint32_t id = key.getId();
    std::atomic<uint32_t>* found = m_words.make(id);
    if (!found) [[unlikely]]
      return refuseId(id);
    const uint32_t was = found->load(std::memory_order_relaxed);
    superseded += was & 1u;
    generation = (was >> 1) + 1;
    found->store(live ? liveWord(generation) : generation << 1,
                 std::memory_order_relaxed);
    return true;
  }

  /**
   * The key's word (liveWord() while its last was a send), 0 for a key never
   * sent to. Any thread.
   * @param id the key's entity id
   * @return the word
   */
  inline uint32_t word(uint32_t id) const
  {
    const std::atomic<uint32_t>* found = m_words.find(id);
    return found ? found->load(std::memory_order_relaxed) : 0u;
  }

  /**
   * Whether an event of this generation is still the key's current one: the
   * newest sent, not cancelled, its entity not destroyed. Any thread.
   * @param id the key's entity id
   * @param generation the event's generation
   * @return whether it is current
   */
  inline bool current(uint32_t id, uint32_t generation) const
  {
    return word(id) == liveWord(generation);
  }

  /**
   * An entity was destroyed: its current event, if any, is superseded.
   * Owning thread only.
   * @param id its id
   */
  void entityRemoved(uint32_t id);

  /**
   * A keyed send or request for an id past PagedWords::kIds (or an invalid
   * handle): logged, refused.
   * @param id the id
   * @return false
   */
  bool refuseId(uint32_t id) const;

  /**
   * A keyed send or request through a stale handle: logged once per type,
   * refused. Owning thread only.
   * @param key the stale key
   * @return false
   */
  bool refuseStale(const Entity& key);

  /**
   * A request type's requests (null for other types): one ticket per
   * requester and entity asked about, made with its queue.
   */
  std::unique_ptr<RequestTickets> tickets;

  /**
   * The inbox of a response type, made the first time it's asked for. Call it
   * under `mutex`.
   * @param type the response type's id
   * @param adopt null when the response type's queue asks (it takes the inbox
   *        now); otherwise how the owning thread makes that queue later
   * @return the inbox, or null once the channel is closed
   */
  template <typename TResponse>
  std::shared_ptr<EventInbox<TResponse>> responseInbox(int type,
                                                       void (*adopt)(Events&))
  {
    for (ResponseSlot& slot : slots)
      if (slot.type == type)
      {
        if (!adopt)
          slot.adopted = true;
        return std::static_pointer_cast<EventInbox<TResponse>>(slot.inbox);
      }
    if (!open)
      return nullptr;
    std::shared_ptr<EventInbox<TResponse>> inbox =
        std::make_shared<EventInbox<TResponse>>();
    slots.push_back({type, inbox, adopt, adopt == nullptr});
    if (adopt)
      unadopted.store(true, std::memory_order_release);
    return inbox;
  }

  /**
   * Takes the inboxes workers made before their response types' queues: the
   * adopt functions to call (after unlocking). Owning thread only.
   * @param out gains each one's adopt function
   */
  void takeUnadopted(std::vector<void (*)(Events&)>& out);

  /**
   * Closes the channel as its events go: responders then fail, and so do the
   * response inboxes. Owning thread only.
   */
  void close();

  /** Guards `slots` and `open`. */
  std::mutex mutex;
  /** The response types' inboxes, in the order first asked for. */
  std::vector<ResponseSlot> slots;
  /** False once the events are gone. */
  bool open = true;
  /** Whether a worker made an inbox the owning thread hasn't adopted. */
  std::atomic<bool> unadopted{false};
  /**
   * The owning thread's id, so a responder on it responds at once; 0 once
   * closed.
   */
  std::atomic<std::uintptr_t> owner{0};
  /** The events the channel belongs to; read only by the owning thread. */
  Events* events = nullptr;
  /**
   * Current events superseded by a newer send, a cancel or their entity's
   * destruction, in all. Owning thread only.
   */
  uint64_t superseded = 0;
  /** The event type's name, for the log. */
  std::string_view name;
  /**
   * The events' table of each entity id's least living generation (set as
   * an entity is destroyed), for stale(); null before any keyed type. Owning
   * thread only.
   */
  const PagedWords<uint32_t>* keyGenerations = nullptr;

private:
  bool m_warnedStale = false;

  PagedWords<std::atomic<uint32_t>> m_words;
};

} // namespace sfs::detail
