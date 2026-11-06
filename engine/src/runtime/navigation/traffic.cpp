#include "engine/runtime/navigation/traffic.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sfs::nav
{

namespace
{
constexpr float kForever = std::numeric_limits<float>::infinity();
constexpr float kTimeSlack = 1.0f;      // s either side a meeting still counts
constexpr float kStandingMargin = 0.0f; // m round one standing still (exact)
glm::vec2 leftOf(glm::vec2 f) { return {-f.y, f.x}; }
} // namespace

bool overlaps(const Footprint& a, const Footprint& b, float margin)
{
  const glm::vec2 fa = unit(a.heading), fb = unit(b.heading);
  const std::array<glm::vec2, 4> axes{fa, leftOf(fa), fb, leftOf(fb)};
  const glm::vec2 d = b.centre - a.centre;
  // (Both grown by half the margin: the same answer either way round.)
  const glm::vec2 ha = a.half + 0.5f * margin, hb = b.half + 0.5f * margin;
  for (const glm::vec2& ax : axes)
  {
    const float ra = ha.x * std::abs(glm::dot(fa, ax)) +
                     ha.y * std::abs(glm::dot(leftOf(fa), ax));
    const float rb = hb.x * std::abs(glm::dot(fb, ax)) +
                     hb.y * std::abs(glm::dot(leftOf(fb), ax));
    if (std::abs(glm::dot(d, ax)) > ra + rb)
      return false;
  }
  return true;
}

void Traffic::update(AgentId id, Claim claim, double now)
{
  m_now = now;
  auto it = m_agents.find(id);
  if (it == m_agents.end() || it->second.claim.path != claim.path)
  {
    // (A new path: a new claim, from now; the orders it was in are decided
    // again.)
    std::erase_if(m_order,
                  [&](const auto& kv)
                  { return kv.first.first == id || kv.first.second == id; });
    m_agents[id] = Agent{std::move(claim), now};
    return;
  }
  it->second.claim = std::move(claim);
}

void Traffic::remove(AgentId id)
{
  m_agents.erase(id);
  std::erase_if(m_order,
                [&](const auto& kv)
                { return kv.first.first == id || kv.first.second == id; });
}

void Traffic::cooperate(AgentId a, AgentId b, bool on)
{
  const auto key = std::minmax(a, b);
  if (on)
    m_cooperating.insert(key);
  else
    m_cooperating.erase(key);
}

bool Traffic::yields(AgentId a, AgentId b) const
{
  const auto key = std::minmax(a, b);
  auto it = m_order.find(key);
  if (it == m_order.end())
  {
    const auto xa = m_agents.find(a), ya = m_agents.find(b);
    if (xa == m_agents.end() || ya == m_agents.end())
      return xa == m_agents.end(); // (an unknown agent never has right of way)
    const Agent& x = xa->second;
    const Agent& y = ya->second;
    AgentId yielder;
    if (x.claim.priority != y.claim.priority)
      yielder = x.claim.priority < y.claim.priority ? a : b;
    else if (x.since != y.since)
      yielder = x.since > y.since ? a : b; // (the later claim gives way)
    else
      yielder = std::max(a, b);
    it = m_order.emplace(key, yielder).first;
  }
  const bool swapped = m_swapped.contains(key);
  return (it->second == a) != swapped;
}

// (One standing still, or parked holding its path, is passed as close as its
// exact boxes allow: planners keep clear of it already.)
float Traffic::bodyMargin(const Claim& c) const
{
  return c.standing || c.parked ? kStandingMargin : m_margin;
}

// The first of a's samples that meets b's claim in time (and space), if any.
bool Traffic::meets(const Agent& a, const Agent& b, std::size_t* first) const
{
  const auto meetsAt = [&](const Sample& s)
  {
    const auto against = [&](const std::vector<Footprint>& boxes,
                             float enter,
                             float exit,
                             float margin)
    {
      if (s.enter > exit + kTimeSlack || enter > s.exit + kTimeSlack)
        return false;
      for (const Footprint& p : s.boxes)
        for (const Footprint& q : boxes)
          if (overlaps(p, q, margin))
            return true;
      return false;
    };
    if (against(b.claim.body,
                0.0f,
                b.claim.standing || b.claim.ahead.empty()
                    ? kForever
                    : b.claim.ahead.front().exit,
                bodyMargin(b.claim)))
      return true;
    if (!b.claim.standing)
      for (const Sample& t : b.claim.ahead)
        if (against(t.boxes, t.enter, t.exit, m_margin))
          return true;
    return false;
  };
  for (std::size_t i = 0; i < a.claim.ahead.size(); ++i)
    if (meetsAt(a.claim.ahead[i]))
    {
      if (first)
        *first = i;
      return true;
    }
  return false;
}

bool Traffic::clearOf(AgentId other, const std::vector<Footprint>& boxes) const
{
  const auto it = m_agents.find(other);
  if (it == m_agents.end())
    return true;
  const Claim& c = it->second.claim;
  for (const Footprint& p : boxes)
  {
    for (const Footprint& q : c.body)
      if (overlaps(p, q, m_margin))
        return false;
    if (!c.standing)
      for (const Sample& s : c.ahead)
        for (const Footprint& q : s.boxes)
          if (overlaps(p, q, m_margin))
            return false;
  }
  return true;
}

std::vector<Footprint> Traffic::claimed(AgentId other) const
{
  std::vector<Footprint> out;
  const auto it = m_agents.find(other);
  if (it == m_agents.end())
    return out;
  const Claim& c = it->second.claim;
  out = c.body;
  if (!c.standing)
    for (const Sample& s : c.ahead)
      out.insert(out.end(), s.boxes.begin(), s.boxes.end());
  return out;
}

Traffic::Advice Traffic::advise(AgentId id) const
{
  Advice out;
  const auto me = m_agents.find(id);
  if (me == m_agents.end() || me->second.claim.standing)
    return out;
  const Agent& a = me->second;
  const auto rank = [](Advice::Kind k)
  {
    return k == Advice::Kind::ClearOut ? 3
           : k == Advice::Kind::Wait   ? 2
           : k == Advice::Kind::HoldAt ? 1
                                       : 0;
  };
  const auto take = [&](Advice v)
  {
    if (rank(v.kind) > rank(out.kind) || (v.kind == out.kind && v.s < out.s))
      out = v;
  };
  for (const auto& [oid, b] : m_agents)
  {
    if (oid == id || m_cooperating.contains(std::minmax(id, oid)))
      continue;
    std::size_t first = 0;
    if (!meets(a, b, &first))
      continue;
    if (!yields(id, oid))
    {
      // (The right of way: it stops only for the other's body on its way.)
      for (std::size_t i = 0; i < a.claim.ahead.size(); ++i)
        for (const Footprint& p : a.claim.ahead[i].boxes)
          for (const Footprint& q : b.claim.body)
            if (overlaps(p, q, bodyMargin(b.claim)))
            {
              const float s = i > 0 ? a.claim.ahead[i - 1].s : a.claim.s;
              take({Advice::Kind::Wait, s, oid});
              i = a.claim.ahead.size();
              break;
            }
      continue;
    }
    // Giving way: the last place before the meeting clear of all the other
    // claims and fit to wait in.
    bool held = false;
    for (std::size_t j = first; j-- > 0;)
    {
      const Sample& s = a.claim.ahead[j];
      if (s.boxes.empty() || !clearOf(oid, s.boxes))
        continue;
      if (m_noWait && m_noWait(s.boxes.front().centre))
        continue;
      take({Advice::Kind::HoldAt, s.s, oid});
      held = true;
      break;
    }
    if (held)
      continue;
    if (clearOf(oid, a.claim.body) && !(m_noWait && !a.claim.body.empty() &&
                                        m_noWait(a.claim.body.front().centre)))
      take({Advice::Kind::HoldAt, a.claim.s, oid});
    else
      take({Advice::Kind::ClearOut, a.claim.s, oid});
  }
  return out;
}

void Traffic::cannotClear(AgentId id, AgentId other)
{
  const auto key = std::minmax(id, other);
  const auto a = m_agents.find(id), b = m_agents.find(other);
  if (a == m_agents.end() || b == m_agents.end())
    return;
  // (Once: the other gives way instead, if it isn't the one with the higher
  // priority; else, or a second time, both stuck: a deadlock, reported.)
  if (!m_swapped.contains(key) &&
      b->second.claim.priority <= a->second.claim.priority)
  {
    m_swapped.insert(key);
    return;
  }
  if (std::none_of(m_deadlocks.begin(),
                   m_deadlocks.end(),
                   [&](const Deadlock& d)
                   { return std::minmax(d.a, d.b) == key; }))
    m_deadlocks.push_back({id, other});
}

} // namespace sfs::nav
