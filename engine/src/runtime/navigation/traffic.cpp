#include "engine/runtime/navigation/traffic.h"

#include "engine/runtime/navigation/vehicle/vehicleModel.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sfs::nav
{

namespace
{
constexpr float kForever = std::numeric_limits<float>::infinity();
constexpr float kTimeSlack = 1.0f; // s either side a meeting still counts
// m short of its stop: it stands there (for traffic), not just slows.
constexpr float kAtStop = 0.5f;
// m round one standing still: the planners' own gap round exact boxes (one
// gap: vehicle::Tunables::gap), never less.
constexpr float kStandingMargin = vehicle::Tunables{}.gap;
glm::vec2 leftOf(glm::vec2 f) { return {-f.y, f.x}; }
using Why = Traffic::Schedule::Why;
} // namespace

bool hits(const std::vector<Footprint>& a,
          const std::vector<Footprint>& b,
          float margin)
{
  for (const Footprint& p : a)
    for (const Footprint& q : b)
      if (overlaps(p, q, margin))
        return true;
  return false;
}

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

void Traffic::update(AgentId id, Claim claim)
{
  m_agents[id].claim = std::move(claim);
}

void Traffic::remove(AgentId id) { m_agents.erase(id); }

void Traffic::cooperate(AgentId a, AgentId b, bool on)
{
  const auto key = std::minmax(a, b);
  if (on)
    m_cooperating.insert(key);
  else
    m_cooperating.erase(key);
}

bool Traffic::cooperating(AgentId a, AgentId b) const
{
  return m_cooperating.contains(std::minmax(a, b));
}

bool Traffic::ranksAbove(AgentId a, AgentId b) const
{
  const auto x = m_agents.find(a), y = m_agents.find(b);
  if (x == m_agents.end() || y == m_agents.end())
    return x != m_agents.end();
  const int pa = x->second.claim.priority, pb = y->second.claim.priority;
  return pa != pb ? pa > pb : a < b;
}

// (One standing still, or a static obstacle, is passed as close as its exact
// boxes allow: planners keep clear of it already.)
float Traffic::bodyMargin(const Claim& c) const
{
  return c.standing || c.solid ? kStandingMargin : m_margin;
}

// Its reservation: its body (till it moves on; for good while it stands) and
// its next seconds up to where it will stand, each in its time window.
bool Traffic::meets(const Sample& s, const Agent& h) const
{
  const Claim& c = h.claim;
  const bool stands =
      c.standing || c.ahead.empty() || h.reservedTo <= c.s + kAtStop;
  const float bodyUntil = stands ? kForever : c.ahead.front().exit;
  if (s.enter <= bodyUntil + kTimeSlack && hits(s.boxes, c.body, bodyMargin(c)))
    return true;
  if (stands)
    return false;
  for (const Sample& t : c.ahead)
  {
    if (t.s > h.reservedTo)
      break;
    if (s.enter > t.exit + kTimeSlack || t.enter > s.exit + kTimeSlack)
      continue;
    if (hits(s.boxes, t.boxes, m_margin))
      return true;
  }
  return false;
}

std::vector<Footprint> Traffic::reservedAbove(AgentId id) const
{
  std::vector<Footprint> out;
  const auto me = m_agents.find(id);
  const AgentId meeting = me == m_agents.end() ? 0 : me->second.schedule.by;
  for (const auto& [oid, h] : m_agents)
  {
    if (oid == id || cooperating(id, oid))
      continue;
    for (const auto& area : h.held)
      out.insert(out.end(), area.begin(), area.end());
    if (!ranksAbove(oid, id) || h.claim.solid)
      continue;
    out.insert(out.end(), h.claim.body.begin(), h.claim.body.end());
    if (h.claim.standing)
      continue;
    for (const Sample& t : h.claim.ahead)
      if (t.s <= h.reservedTo)
        out.insert(out.end(), t.boxes.begin(), t.boxes.end());
    if (oid == meeting)
      out.insert(out.end(), h.claim.way.begin(), h.claim.way.end());
  }
  return out;
}

bool Traffic::clearOfAbove(AgentId id,
                           const std::vector<Footprint>& boxes,
                           AgentId meeting) const
{
  for (const auto& [oid, h] : m_agents)
  {
    if (oid == id || cooperating(id, oid))
      continue;
    for (const auto& area : h.held)
      if (hits(boxes, area))
        return false;
    if (!ranksAbove(oid, id) || h.claim.solid)
      continue;
    if (hits(boxes, h.claim.body, m_margin))
      return false;
    if (h.claim.standing)
      continue;
    for (const Sample& t : h.claim.ahead)
      if (t.s <= h.reservedTo && hits(boxes, t.boxes, m_margin))
        return false;
    if (oid == meeting && hits(boxes, h.claim.way, m_margin))
      return false;
  }
  return true;
}

// Its narrow ground: what it holds (granted, or still under its body) and,
// in order along its way, each narrow stretch it comes to, granted whole
// while nobody else holds ground it overlaps.
void Traffic::holdZones(AgentId id, Agent& a)
{
  const Claim& c = a.claim;
  const float reach = c.ahead.empty() ? c.s : c.ahead.back().s;
  const auto heldByOther = [&](const std::vector<Footprint>& area)
  {
    for (const auto& [oid, o] : m_agents)
      if (oid != id && !cooperating(id, oid))
        for (const auto& other : o.held)
          if (hits(area, other))
            return true;
    return false;
  };
  std::vector<std::vector<Footprint>> held;
  std::vector<float> from;
  if (!c.standing)
    for (const Zone& z : c.zones)
    {
      if (z.from > reach || z.area.empty())
        break;
      const bool mine =
          std::any_of(a.held.begin(),
                      a.held.end(),
                      [&](const auto& area) { return hits(z.area, area); });
      if (!mine && heldByOther(z.area))
        break; // (it waits for it: entering it is stopped below)
      held.push_back(z.area);
      from.push_back(hits(c.body, z.area) ? -kForever : z.from);
    }
  for (auto& area : a.held)
    if (hits(c.body, area) &&
        std::none_of(held.begin(),
                     held.end(),
                     [&](const auto& h) { return hits(h, area); }))
    {
      held.push_back(std::move(area)); // (still in it: held till it's out)
      from.push_back(-kForever);
    }
  a.held = std::move(held);
  a.heldFrom = std::move(from);
}

// Never hold-and-wait: the narrow ground it isn't in yet, beyond where it
// stands, is given up (granted again once it can go on to it).
void Traffic::releaseShort(Agent& a, float stopAt)
{
  for (std::size_t i = a.held.size(); i-- > 0;)
    if (a.heldFrom[i] > stopAt)
    {
      a.held.erase(a.held.begin() + static_cast<std::ptrdiff_t>(i));
      a.heldFrom.erase(a.heldFrom.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void Traffic::scheduleOne(AgentId id,
                          Agent& a,
                          const std::vector<AgentId>& above)
{
  Schedule out;
  holdZones(id, a);
  const Claim& c = a.claim;
  if (c.standing)
  {
    a.schedule = out;
    return;
  }
  const auto rank = [](Why w)
  {
    return w == Why::Solid      ? 4
           : w == Why::Reserved ? 3
           : w == Why::Zone     ? 2
           : w == Why::Body     ? 1
                                : 0;
  };
  const auto take = [&](Why why, float s, AgentId by)
  {
    if (s < out.stopAt || (s == out.stopAt && rank(why) > rank(out.why)))
    {
      out.why = why;
      out.stopAt = s;
      out.by = by;
    }
  };
  const auto before = [&](std::size_t i)
  { return i > 0 ? c.ahead[i - 1].s : c.s; };

  for (const auto& [oid, o] : m_agents)
  {
    if (oid == id || cooperating(id, oid))
      continue;
    // Never on into another's body, whatever the ranks; a static obstacle
    // on its way is planned round.
    for (std::size_t i = 0; i < c.ahead.size(); ++i)
      if (hits(c.ahead[i].boxes, o.claim.body, bodyMargin(o.claim)))
      {
        take(o.claim.solid ? Why::Solid : Why::Body, before(i), oid);
        break;
      }
    // Never into narrow ground another holds (once in it, on through).
    for (const auto& area : o.held)
    {
      if (c.leaving == oid || hits(c.body, area))
        continue;
      for (std::size_t i = 0; i < c.ahead.size(); ++i)
        if (hits(c.ahead[i].boxes, area))
        {
          take(Why::Zone, before(i), oid);
          break;
        }
    }
  }

  // Out of every higher reservation: where its way first meets one, it
  // stands at the last point before that clear of them all. (Not one that
  // stands for its body, or for the narrow place it holds: that one waits
  // for it to go on, so it goes first.)
  const auto waitsForMe = [&](AgentId h)
  {
    const Schedule& hs = m_agents.at(h).schedule;
    return (hs.why == Why::Body || hs.why == Why::Zone) && hs.by == id;
  };
  std::size_t first = c.ahead.size();
  AgentId by = 0;
  for (std::size_t i = 0; i < c.ahead.size() && !by; ++i)
    for (const AgentId h : above)
      if (!cooperating(id, h) && !m_agents.at(h).claim.solid &&
          !waitsForMe(h) && meets(c.ahead[i], m_agents.at(h)))
      {
        first = i;
        by = h;
        break;
      }
  bool nowhere = false;
  if (by)
  {
    bool held = false;
    for (std::size_t j = first; j-- > 0;)
    {
      const Sample& s = c.ahead[j];
      if (s.boxes.empty() || !clearOfAbove(id, s.boxes, by) ||
          (m_noWait && m_noWait(s.boxes.front().centre)))
        continue;
      take(Why::Reserved, s.s, by);
      held = true;
      break;
    }
    if (!held)
    {
      take(Why::Reserved, c.s, by);
      nowhere =
          !clearOfAbove(id, c.body, by) ||
          (m_noWait && !c.body.empty() && m_noWait(c.body.front().centre));
    }
  }
  // Standing for traffic (or for its body: head-on) in the way of a higher
  // one that stands for its body, or held by the body of one that waits for
  // its narrow place: it must move off (else each waits on the other).
  const bool standsHere = out.why != Why::None && out.stopAt <= c.s + kAtStop;
  for (const AgentId h : above)
  {
    const Schedule& hs = m_agents.at(h).schedule;
    const bool forBody = hs.why == Why::Body && hs.by == id;
    if (waitsForMe(h) && !cooperating(id, h) &&
        ((forBody && standsHere) || (out.why == Why::Body && out.by == h)))
    {
      nowhere = true;
      if (out.why != Why::Reserved)
        out.by = h;
      break;
    }
  }
  out.inTheWay = nowhere;
  a.schedule = out;
  if (out.why != Why::None)
    releaseShort(a, out.stopAt);
}

void Traffic::schedule()
{
  std::vector<AgentId> order;
  order.reserve(m_agents.size());
  for (const auto& [id, a] : m_agents)
    order.push_back(id);
  std::sort(order.begin(),
            order.end(),
            [&](AgentId a, AgentId b) { return ranksAbove(a, b); });
  std::vector<AgentId> above;
  for (const AgentId id : order)
  {
    Agent& a = m_agents.at(id);
    scheduleOne(id, a, above);
    // (It reserves only up to where it will stand.)
    a.reservedTo = a.schedule.why == Why::None ? kForever : a.schedule.stopAt;
    above.push_back(id);
  }
}

Traffic::Schedule Traffic::scheduleOf(AgentId id) const
{
  const auto it = m_agents.find(id);
  return it == m_agents.end() ? Schedule{} : it->second.schedule;
}

std::vector<Footprint> Traffic::heldBesides(AgentId id) const
{
  std::vector<Footprint> out;
  for (const auto& [oid, o] : m_agents)
    if (oid != id && !cooperating(id, oid))
      for (const auto& area : o.held)
        out.insert(out.end(), area.begin(), area.end());
  return out;
}

bool Traffic::holdsZone(AgentId id) const
{
  const auto it = m_agents.find(id);
  return it != m_agents.end() && !it->second.held.empty();
}

} // namespace sfs::nav
