#include "engine/core/events/requestTickets.h"

#include "engine/core/logger/logger.h"

#include <limits>
#include <string>

namespace sfs::detail
{

uint32_t RequestTickets::addRequester()
{
  if (!m_freeNumbers.empty())
  {
    const uint32_t number = m_freeNumbers.back();
    m_freeNumbers.pop_back();
    return number;
  }
  m_requesterHeads.emplace_back();
  return static_cast<uint32_t>(m_requesterHeads.size());
}

void RequestTickets::removeRequester(uint32_t requester)
{
  if (requester == 0 || requester > m_requesterHeads.size())
    return;
  uint32_t next = m_requesterHeads[requester - 1].first;
  while (next != 0)
  {
    const uint32_t ticket = next - 1;
    RequestTicket& held = at(ticket);
    next = held.requesterNext;
    superseded += held.word.load(std::memory_order_relaxed) & 1u;
    unlinkEntity(ticket);
    free(ticket);
  }
  m_requesterHeads[requester - 1] = {};
  m_freeNumbers.push_back(requester);
}

bool RequestTickets::ask(uint32_t requester,
                         uint32_t entity,
                         uint32_t& ticket,
                         uint32_t& generation)
{
  m_generation = (m_generation + 1) & 0x7fffffffu;
  if (m_generation == 0)
    m_generation = 1;
  generation = m_generation;
  if (const uint32_t found = lookup(requester, entity))
  {
    ticket = found - 1;
    RequestTicket& held = at(ticket);
    const uint32_t word = held.word.load(std::memory_order_relaxed);
    superseded += word & 1u;
    // Open again: back among the entity's open tickets (unless it's first).
    if (!isOpen(held, word) && held.entityPrev != 0)
      if (Head* head = m_entityHeads.find(entity))
      {
        unlinkEntity(ticket);
        ++head->count;
        linkEntityFirst(ticket, *head);
      }
    held.responded = 0;
    held.word.store(liveWord(generation), std::memory_order_relaxed);
    return true;
  }
  Head* entityHead = m_entityHeads.make(entity);
  ticket = entityHead ? allocate() : std::numeric_limits<uint32_t>::max();
  if (ticket == std::numeric_limits<uint32_t>::max()) [[unlikely]]
  {
    if (!m_warnedFull)
      LOG_ERROR("Events: a request type has " + std::to_string(kTickets) +
                " requests outstanding, so a request was dropped.");
    m_warnedFull = true;
    return false;
  }
  RequestTicket& made = at(ticket);
  made.requester = requester;
  made.entity = entity;
  made.responded = 0;
  linkEntityFirst(ticket, *entityHead);
  ++entityHead->count;
  Head& requesterHead = m_requesterHeads[requester - 1];
  made.requesterPrev = 0;
  made.requesterNext = requesterHead.first;
  if (requesterHead.first != 0)
    at(requesterHead.first - 1).requesterPrev = ticket + 1;
  requesterHead.first = ticket + 1;
  ++requesterHead.count;
  made.word.store(liveWord(generation), std::memory_order_relaxed);
  return true;
}

bool RequestTickets::cancel(uint32_t requester, uint32_t entity)
{
  const uint32_t found = lookup(requester, entity);
  if (found == 0)
    return false;
  superseded += at(found - 1).word.load(std::memory_order_relaxed) & 1u;
  unlinkEntity(found - 1);
  unlinkRequester(found - 1);
  free(found - 1);
  return true;
}

bool RequestTickets::waiting(uint32_t requester, uint32_t entity) const
{
  const uint32_t found = lookup(requester, entity);
  if (found == 0)
    return false;
  const RequestTicket& held = *find(found - 1);
  const uint32_t word = held.word.load(std::memory_order_relaxed);
  return (word & 1u) && held.responded != (word >> 1);
}

void RequestTickets::responded(uint32_t ticket, uint32_t generation)
{
  if (!current(ticket, generation) || closed(ticket, generation))
    return;
  RequestTicket& held = at(ticket);
  held.responded = generation;
  // Closed: behind the entity's open tickets (unless it's last).
  if (held.entityNext == 0)
    return;
  if (Head* head = m_entityHeads.find(held.entity))
  {
    unlinkEntity(ticket);
    ++head->count;
    linkEntityLast(ticket, *head);
  }
}

void RequestTickets::entityRemoved(uint32_t entity)
{
  Head* head = m_entityHeads.find(entity);
  if (!head)
    return;
  uint32_t next = head->first;
  *head = {};
  while (next != 0)
  {
    const uint32_t ticket = next - 1;
    RequestTicket& held = at(ticket);
    next = held.entityNext;
    superseded += held.word.load(std::memory_order_relaxed) & 1u;
    unlinkRequester(ticket);
    free(ticket);
  }
}

bool RequestTickets::open(uint32_t entity) const
{
  const Head* head = m_entityHeads.find(entity);
  if (!head || head->first == 0)
    return false;
  const RequestTicket& first = *find(head->first - 1);
  return isOpen(first, first.word.load(std::memory_order_relaxed));
}

std::size_t RequestTickets::bytes() const
{
  std::size_t total = sizeof(*this) +
                      m_requesterHeads.capacity() * sizeof(Head) +
                      m_freeNumbers.capacity() * sizeof(uint32_t);
  for (const std::unique_ptr<RequestTicket[]>& block : m_blocks)
    if (block)
      total += kBlockSize * sizeof(RequestTicket);
  return total + m_entityHeads.bytes();
}

uint32_t RequestTickets::lookup(uint32_t requester, uint32_t entity) const
{
  const Head* entityHead = m_entityHeads.find(entity);
  if (!entityHead || entityHead->count == 0 || requester == 0 ||
      requester > m_requesterHeads.size())
    return 0;
  const Head& requesterHead = m_requesterHeads[requester - 1];
  // Through the shorter list.
  if (entityHead->count <= requesterHead.count)
  {
    for (uint32_t next = entityHead->first; next != 0;)
    {
      const RequestTicket& held = *find(next - 1);
      if (held.requester == requester)
        return next;
      next = held.entityNext;
    }
    return 0;
  }
  for (uint32_t next = requesterHead.first; next != 0;)
  {
    const RequestTicket& held = *find(next - 1);
    if (held.entity == entity)
      return next;
    next = held.requesterNext;
  }
  return 0;
}

uint32_t RequestTickets::allocate()
{
  if (m_freeHead != 0)
  {
    const uint32_t ticket = m_freeHead - 1;
    m_freeHead = at(ticket).entityNext;
    return ticket;
  }
  if (m_made >= kTickets) [[unlikely]]
    return std::numeric_limits<uint32_t>::max();
  std::unique_ptr<RequestTicket[]>& block = m_blocks[m_made >> kBlockBits];
  if (!block)
    block = std::make_unique<RequestTicket[]>(kBlockSize);
  return m_made++;
}

void RequestTickets::unlinkRequester(uint32_t ticket)
{
  RequestTicket& held = at(ticket);
  Head& head = m_requesterHeads[held.requester - 1];
  if (held.requesterPrev != 0)
    at(held.requesterPrev - 1).requesterNext = held.requesterNext;
  else
    head.first = held.requesterNext;
  if (held.requesterNext != 0)
    at(held.requesterNext - 1).requesterPrev = held.requesterPrev;
  --head.count;
  held.requesterPrev = held.requesterNext = 0;
}

void RequestTickets::unlinkEntity(uint32_t ticket)
{
  RequestTicket& held = at(ticket);
  Head* head = m_entityHeads.find(held.entity);
  if (held.entityPrev != 0)
    at(held.entityPrev - 1).entityNext = held.entityNext;
  else if (head)
    head->first = held.entityNext;
  if (held.entityNext != 0)
    at(held.entityNext - 1).entityPrev = held.entityPrev;
  else if (head)
    head->last = held.entityPrev;
  if (head)
    --head->count;
  held.entityPrev = held.entityNext = 0;
}

void RequestTickets::linkEntityFirst(uint32_t ticket, Head& head)
{
  RequestTicket& held = at(ticket);
  held.entityPrev = 0;
  held.entityNext = head.first;
  if (head.first != 0)
    at(head.first - 1).entityPrev = ticket + 1;
  else
    head.last = ticket + 1;
  head.first = ticket + 1;
}

void RequestTickets::linkEntityLast(uint32_t ticket, Head& head)
{
  RequestTicket& held = at(ticket);
  held.entityNext = 0;
  held.entityPrev = head.last;
  if (head.last != 0)
    at(head.last - 1).entityNext = ticket + 1;
  else
    head.first = ticket + 1;
  head.last = ticket + 1;
}

void RequestTickets::free(uint32_t ticket)
{
  RequestTicket& held = at(ticket);
  held.word.store(0, std::memory_order_relaxed);
  held.requester = 0;
  held.entityPrev = 0;
  held.entityNext = m_freeHead;
  m_freeHead = ticket + 1;
}

} // namespace sfs::detail
