#pragma once

#include "engine/core/events/keyed.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace sfs::detail
{

/**
 * One requester's current request about one entity: a request type's ticket.
 * Rows of the request and of its responses name it with its generation.
 */
struct RequestTicket
{
  /**
   * liveWord(generation) while the request is current; anything else once
   * it's superseded, cancelled or freed. Read from any thread.
   */
  std::atomic<uint32_t> word{0};
  /** The requester's number. Owning thread only, like the rest. */
  uint32_t requester = 0;
  /** The entity id the request is about. */
  uint32_t entity = 0;
  /** The generation whose final response was taken in (waiting()). */
  uint32_t responded = 0;
  /** The entity's tickets, as a list (+1; 0: none); the next free one. */
  uint32_t entityNext = 0;
  /** @copydoc entityNext */
  uint32_t entityPrev = 0;
  /** The requester's tickets, as a list (+1; 0: none). */
  uint32_t requesterPrev = 0;
  /** @copydoc requesterPrev */
  uint32_t requesterNext = 0;
};

/**
 * A request type's requests, one ticket per (requester, entity) asked about
 * and not yet superseded by the entity's or the requester's going, or a
 * cancel: memory follows the requests outstanding, not the requesters. Each
 * request gets a generation unique to the type, so a row whose ticket was
 * freed and reused never matches. Tickets live in blocks that never move, so
 * a responder on any thread can read a ticket's word; the rest is the owning
 * thread's. An entity's tickets are listed from its id, so destroying it
 * touches only those; a requester's from its number, so its going touches
 * only its own. Both lists are doubly linked and counted: a requester's
 * ticket about an entity is found through the shorter, so a requester asking
 * about one entity, or an entity asked about by one requester, costs the
 * same however many others there are. An entity's open tickets come before
 * its closed ones (a ticket goes to the back as its final response is taken
 * in, to the front as it's asked again), so answering by entity walks only
 * the open ones. Nothing allocates once the blocks and pages are made.
 */
class RequestTickets
{
public:
  /**
   * A new requester's number (from 1; a gone requester's number may come
   * back, as its tickets went with it).
   * @return the number
   */
  uint32_t addRequester();

  /**
   * A requester asks about an entity: its ticket for the entity (made the
   * first time) gets the type's next generation, superseding its last.
   * @param requester its number
   * @param entity the entity id
   * @param ticket set to the ticket
   * @param generation set to the request's generation
   * @return false when the type's tickets are all taken (logged once)
   */
  bool ask(uint32_t requester,
           uint32_t entity,
           uint32_t& ticket,
           uint32_t& generation);

  /**
   * The memory it holds, in bytes: its blocks, pages and lists.
   * @return the bytes
   */
  std::size_t bytes() const;

  /**
   * Cancels a requester's current request about an entity.
   * @param requester its number
   * @param entity the entity id
   * @return whether there was one
   */
  bool cancel(uint32_t requester, uint32_t entity);

  /**
   * Whether the request of `generation` had its final response: it's
   * closed, and takes no more. Owning thread only.
   * @param ticket the ticket, current (current())
   * @param generation the request's generation
   * @return whether it's closed
   */
  inline bool closed(uint32_t ticket, uint32_t generation) const
  {
    return find(ticket)->responded == generation;
  }

  /**
   * Whether `ticket` still holds the request of `generation`. Any thread.
   * @param ticket the ticket
   * @param generation the request's generation
   * @return whether the request is current
   */
  inline bool current(uint32_t ticket, uint32_t generation) const
  {
    const RequestTicket* found = find(ticket);
    return found &&
           found->word.load(std::memory_order_relaxed) == liveWord(generation);
  }

  /**
   * An entity is destroyed: its tickets, every requester's, are freed.
   * @param entity its id
   */
  void entityRemoved(uint32_t entity);

  /**
   * Calls `fn(ticket, generation)` for each current request about an entity
   * that has no final response yet, every requester's: what a server
   * answering by entity reaches. Walks only the entity's open tickets, which
   * come first; `fn` may close the one it's given (responded()). Owning
   * thread only.
   * @param entity the entity id
   * @param fn what to call
   */
  template <typename TFunction>
  void forEachOpen(uint32_t entity, TFunction&& fn)
  {
    const Head* head = m_entityHeads.find(entity);

    for (uint32_t next = head ? head->first : 0; next != 0;)
    {
      const uint32_t ticket = next - 1;
      const RequestTicket& held = *find(ticket);
      const uint32_t word = held.word.load(std::memory_order_relaxed);

      if (!isOpen(held, word))
        return;

      next = held.entityNext;
      fn(ticket, word >> 1);
    }
  }

  /**
   * Whether any requester's current request about an entity has no final
   * response yet. Owning thread only.
   * @param entity the entity id
   * @return whether one is open
   */
  bool open(uint32_t entity) const;

  /**
   * A requester goes: its tickets are freed, so its requests and their
   * responses are superseded.
   * @param requester its number
   */
  void removeRequester(uint32_t requester);

  /**
   * The requester a current ticket belongs to. Owning thread only.
   * @param ticket the ticket, current (current())
   * @return its requester's number
   */
  inline uint32_t requesterOf(uint32_t ticket) const
  {
    return find(ticket)->requester;
  }

  /**
   * The final response to a ticket's request was taken in.
   * @param ticket the ticket
   * @param generation the request's generation
   */
  void responded(uint32_t ticket, uint32_t generation);

  /**
   * Whether a requester's current request about an entity has no final
   * response yet.
   * @param requester its number
   * @param entity the entity id
   * @return whether it waits
   */
  bool waiting(uint32_t requester, uint32_t entity) const;

  /** Tickets a block holds, as a power of two. */
  static constexpr uint32_t kBlockBits = 8;
  /** Tickets a block holds. */
  static constexpr uint32_t kBlockSize = 1u << kBlockBits;
  /** The most tickets a request type has at once. */
  static constexpr uint32_t kTickets = kBlockSize * 4096;
  /** Current requests superseded or cancelled, in all. */
  uint64_t superseded = 0;

private:
  // A list's first and last tickets (+1) and its length (an entity's last
  // is kept; a requester's isn't needed).
  struct Head
  {
    uint32_t first = 0;
    uint32_t last = 0;
    uint32_t count = 0;
  };

  // A free ticket (UINT32_MAX when all are taken).
  uint32_t allocate();
  inline RequestTicket& at(uint32_t ticket)
  {
    return m_blocks[ticket >> kBlockBits][ticket & (kBlockSize - 1)];
  }
  // The ticket, if its block is made.
  inline const RequestTicket* find(uint32_t ticket) const
  {
    if (ticket >= kTickets) [[unlikely]]
      return nullptr;

    const RequestTicket* block = m_blocks[ticket >> kBlockBits].get();
    return block ? block + (ticket & (kBlockSize - 1)) : nullptr;
  }
  // Puts an unlinked ticket on the free list, its word cleared.
  void free(uint32_t ticket);
  // Whether a linked ticket, its word read, is open: current with no final
  // response taken in.
  static inline bool isOpen(const RequestTicket& held, uint32_t word)
  {
    return (word & 1u) && held.responded != (word >> 1);
  }
  // Puts an unlinked ticket first, or last, in its entity's list.
  void linkEntityFirst(uint32_t ticket, Head& head);
  void linkEntityLast(uint32_t ticket, Head& head);
  // A requester's ticket about an entity, +1; 0 for none.
  uint32_t lookup(uint32_t requester, uint32_t entity) const;
  void unlinkEntity(uint32_t ticket);
  // Takes a ticket out of its requester's list, or its entity's.
  void unlinkRequester(uint32_t ticket);

  std::array<std::unique_ptr<RequestTicket[]>, kTickets / kBlockSize> m_blocks;
  uint32_t m_made = 0;     // tickets ever handed out (the next new one)
  uint32_t m_freeHead = 0; // freed tickets, +1, through entityNext
  // By entity id: its tickets.
  PagedWords<Head> m_entityHeads;
  // By requester number - 1: its tickets.
  std::vector<Head> m_requesterHeads;
  std::vector<uint32_t> m_freeNumbers;
  uint32_t m_generation = 0;
  bool m_warnedFull = false;
};

} // namespace sfs::detail
