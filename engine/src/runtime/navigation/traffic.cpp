#include "engine/runtime/navigation/traffic.h"

#include "engine/runtime/navigation/vehicle/vehicleModel.h"

#include "glm/glm/vector_relational.hpp"

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
// m a way out of a touch it stands in isn't held by what it touches (the
// planner's own let-off: the way was planned out of it within this).
constexpr float kEscape = vehicle::kEscapeReach;
// m/s: easing off slower than this (a walking pace) it just stands at its
// stop and waits (a behaviour setting, not physics).
constexpr float kEaseLeast = 1.0f;
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
// boxes allow: planners keep clear of it already. One not moving too: a
// wait spot planned past it at the planners' gap is never stood short of.)
float Traffic::bodyMargin(const Claim& c) const
{
  return c.standing || c.solid || c.still ? kStandingMargin : m_margin;
}

// Its reservation: its body (till it moves on; for good while it stands) and
// its next seconds up to where it will stand, each in its time window.
bool Traffic::meets(const Sample& s, const Agent& h, bool body) const
{
  const Claim& c = h.claim;
  const bool stands =
      c.standing || c.ahead.empty() || h.reservedTo <= c.s + kAtStop;
  const float bodyUntil = stands ? kForever : c.ahead.front().exit;
  if (body && s.enter <= bodyUntil + kTimeSlack &&
      hits(s.boxes, c.body, bodyMargin(c)))
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

// Standing still touching the body of one standing still too: its way out
// (planned out of the touch, as the planner lets any way out within
// kEscapeReach) isn't held by that body over its first kEscape metres, only
// past them. (Never one moving: that one's passed at the moving margin.)
bool Traffic::escaping(const Claim& c, const Claim& o) const
{
  return c.still && (o.standing || o.solid || o.still) &&
         hits(c.body, o.body, kStandingMargin);
}

bool Traffic::inGround(const std::vector<Footprint>& boxes,
                       const Claim& h) const
{
  if (hits(boxes, h.body, m_margin))
    return true;
  if (h.standing)
    return false;
  for (const Sample& t : h.ahead)
    if (hits(boxes, t.boxes, m_margin))
      return true;
  return hits(boxes, h.way, m_margin);
}

// (Moving through it, or leaving it for a wait spot: committed to getting
// out; held standing there it would only be in that one's way.)
std::size_t Traffic::wayOut(const Claim& c, AgentId hid, const Claim& h) const
{
  if (h.standing || h.solid || c.standing || c.ahead.empty())
    return 0;
  const bool leaving = c.leavingAll || c.leaving == hid;
  if (c.still && !leaving)
    return 0;
  const std::size_t n =
      c.here > 0 && c.here <= c.body.size() ? c.here : c.body.size();
  // (Where it is, or the sample it's on: a stop there is a stop in it.)
  const bool in = inGround({c.body.begin(), c.body.begin() + n}, h) ||
                  inGround(c.ahead.front().boxes, h);
  if (leaving)
  {
    // (Off to a wait spot clear of that one's way: its way out is all of
    // its way there up to the last of it in that one's ground.)
    for (std::size_t i = c.ahead.size(); i-- > 0;)
      if (inGround(c.ahead[i].boxes, h))
        return std::min(i + 2, c.ahead.size());
    return in ? 1 : 0;
  }
  if (!in)
    return 0;
  for (std::size_t i = 0; i < c.ahead.size(); ++i)
    if (!inGround(c.ahead[i].boxes, h))
      return i + 1;
  // (Not out within its next seconds: it's going along that one's way, not
  // out of it.)
  return 0;
}

bool Traffic::meetsPlanned(const Sample& s, const Claim& h) const
{
  const bool stands = h.standing || h.ahead.empty();
  const float bodyUntil = stands ? kForever : h.ahead.front().exit;
  if (s.enter <= bodyUntil + kTimeSlack && hits(s.boxes, h.body, bodyMargin(h)))
    return true;
  if (stands)
    return false;
  for (const Sample& t : h.ahead)
    if (s.enter <= t.exit + kTimeSlack && t.enter <= s.exit + kTimeSlack &&
        hits(s.boxes, t.boxes, m_margin))
      return true;
  // (Its way on past its next seconds: there from when they end.)
  return s.exit + kTimeSlack >= h.ahead.back().enter &&
         hits(s.boxes, h.way, m_margin);
}

Traffic::Crossing
Traffic::crossing(AgentId id, const Claim& way, bool onTheWay) const
{
  std::vector<std::pair<AgentId, const Agent*>> above;
  for (const auto& [oid, h] : m_agents)
    if (oid != id && !cooperating(id, oid) && ranksAbove(oid, id) &&
        !h.claim.solid)
      above.push_back({oid, &h});
  std::vector<std::size_t> out(above.size());
  for (std::size_t k = 0; k < above.size(); ++k)
    out[k] = wayOut(way, above[k].first, above[k].second->claim);
  // (Clear of them all, the narrow ground others hold, and not a no-wait
  // place: it may stand there and wait.)
  const auto clear = [&](const std::vector<Footprint>& boxes)
  {
    if (boxes.empty() || (m_noWait && m_noWait(boxes.front().centre)))
      return false;
    for (const auto& [oid, h] : above)
      if (inGround(boxes, h->claim))
        return false;
    for (const auto& [oid, o] : m_agents)
      if (oid != id && !cooperating(id, oid))
        for (const auto& area : o.held)
          if (hits(boxes, area))
            return false;
    return true;
  };
  // (One standing for another's body, zone or reservation comes on only
  // once that one's gone: its reservation as it stands now, up to its stop.
  // Otherwise (on its way, or standing for this one: it goes on once this
  // one's out of its way) as planned, all of it.)
  const auto meetsAbove = [&](const Sample& s, const Agent& h)
  {
    const Schedule& hs = h.schedule;
    return hs.why != Why::None && hs.by != id && std::isfinite(h.reservedTo)
               ? meets(s, h)
               : meetsPlanned(s, h.claim);
  };
  for (std::size_t i = 0; i < way.ahead.size(); ++i)
    for (std::size_t k = 0; k < above.size(); ++k)
    {
      if (i < out[k] || !meetsAbove(way.ahead[i], *above[k].second))
        continue;
      for (std::size_t j = i; j-- > 0;)
        if ((onTheWay || j == 0) && clear(way.ahead[j].boxes))
          return {};
      return {static_cast<int>(i), above[k].first};
    }
  return {};
}

// (A wait spot is clear of all the way each higher one has yet to go, not
// just up to where it stands for now: never where one's still to come, its
// goal included.)
std::vector<Footprint> Traffic::reservedAbove(AgentId id) const
{
  std::vector<Footprint> out;
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
      out.insert(out.end(), t.boxes.begin(), t.boxes.end());
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
    const bool out = escaping(c, o.claim);
    for (std::size_t i = 0; i < c.ahead.size(); ++i)
    {
      if (out && c.ahead[i].s - c.s <= kEscape)
        continue;
      if (hits(c.ahead[i].boxes, o.claim.body, bodyMargin(o.claim)))
      {
        take(o.claim.solid ? Why::Solid : Why::Body, before(i), oid);
        break;
      }
    }
    // One below on its way out of its way: it stands short of that way out
    // where they'd meet in time, as for a body.
    // (Not where it's on that way out already: it can't stand short of
    // that; it goes on, as its own way says.)
    const auto w = m_wayOut.find({oid, id});
    const auto onIt = [&]
    {
      for (std::size_t j = 0; j < w->second; ++j)
        if (hits(c.body, o.claim.ahead[j].boxes, m_margin))
          return true;
      return false;
    };
    if (w != m_wayOut.end() && !onIt())
    {
      const Claim& oc = o.claim;
      bool met = false;
      for (std::size_t i = 0; i < c.ahead.size() && !met; ++i)
        for (std::size_t j = 0; j < w->second && !met; ++j)
        {
          const Sample& t = oc.ahead[j];
          if (c.ahead[i].enter > t.exit + kTimeSlack ||
              t.enter > c.ahead[i].exit + kTimeSlack ||
              !hits(c.ahead[i].boxes, t.boxes, bodyMargin(oc)))
            continue;
          take(Why::Body, before(i), oid);
          met = true;
        }
    }
    // Never into narrow ground another holds (once in it, on through).
    for (const auto& area : o.held)
    {
      if (c.leavingAll || c.leaving == oid || hits(c.body, area))
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
          !waitsForMe(h) && !m_wayOut.contains({id, h}) &&
          meets(c.ahead[i],
                m_agents.at(h),
                !(c.ahead[i].s - c.s <= kEscape &&
                  escaping(c, m_agents.at(h).claim))))
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
  // Its stop short of a crossing the higher ones will have cleared in time:
  // it eases off to get there as it clears, rather than braking to a stand.
  if (by && !nowhere && out.why == Why::Reserved && out.by == by &&
      out.stopAt > c.s + kAtStop)
  {
    const float clear = clearIn(id, c, first, above);
    if (std::isfinite(clear) && clear > 0.0f)
    {
      const float v = (out.stopAt - c.s) / clear;
      if (v >= kEaseLeast)
        out.easeTo = v;
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
  // (Who's on its way out of whose way, this frame: each pair whose
  // grounds' bounds meet.)
  m_wayOut.clear();
  std::vector<std::pair<glm::vec2, glm::vec2>> bounds(order.size());
  for (std::size_t k = 0; k < order.size(); ++k)
  {
    const Claim& c = m_agents.at(order[k]).claim;
    glm::vec2 lo(kForever), hi(-kForever);
    const auto grow = [&](const std::vector<Footprint>& boxes)
    {
      for (const Footprint& f : boxes)
      {
        const float r = glm::length(f.half) + m_margin;
        lo = glm::min(lo, f.centre - r);
        hi = glm::max(hi, f.centre + r);
      }
    };
    grow(c.body);
    for (const Sample& t : c.ahead)
      grow(t.boxes);
    grow(c.way);
    bounds[k] = {lo, hi};
  }
  for (std::size_t lo = 0; lo < order.size(); ++lo)
    for (std::size_t hi = 0; hi < lo; ++hi)
    {
      const AgentId o = order[lo], h = order[hi];
      if (cooperating(o, h) || m_agents.at(h).claim.solid ||
          glm::any(glm::lessThan(bounds[lo].second, bounds[hi].first)) ||
          glm::any(glm::lessThan(bounds[hi].second, bounds[lo].first)))
        continue;
      if (const std::size_t n =
              wayOut(m_agents.at(o).claim, h, m_agents.at(h).claim))
        m_wayOut[{o, h}] = n;
    }
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

float Traffic::clearIn(AgentId id,
                       const Claim& c,
                       std::size_t first,
                       const std::vector<AgentId>& above) const
{
  float clear = 0.0f;
  for (const AgentId hid : above)
  {
    const Agent& h = m_agents.at(hid);
    const Claim& hc = h.claim;
    if (cooperating(id, hid) || hc.solid)
      continue;
    bool met = false;
    for (std::size_t j = first; j < c.ahead.size() && !met; ++j)
      met = meets(c.ahead[j], h);
    if (!met)
      continue;
    // (One that will stand on its way (at its stop, or for good), or whose
    // way meets ours past its timed seconds: no telling when it's clear.)
    if (hc.standing || hc.still || hc.ahead.empty() ||
        std::isfinite(h.reservedTo))
      return kForever;
    for (std::size_t j = first; j < c.ahead.size(); ++j)
    {
      const std::vector<Footprint>& boxes = c.ahead[j].boxes;
      if (hits(boxes, hc.way, m_margin))
        return kForever;
      if (hits(boxes, hc.body, bodyMargin(hc)))
        clear = std::max(clear, hc.ahead.front().exit + kTimeSlack);
      for (const Sample& t : hc.ahead)
        if (hits(boxes, t.boxes, m_margin))
          clear = std::max(clear, t.exit + kTimeSlack);
    }
  }
  return clear;
}

Traffic::AgentId Traffic::waitsOn(AgentId id) const
{
  const auto it = m_agents.find(id);
  if (it == m_agents.end())
    return 0;
  const Agent& a = it->second;
  const Schedule& sc = a.schedule;
  if (a.claim.standing || !a.claim.still || !sc.by || !has(sc.by))
    return 0;
  if (sc.why != Why::Reserved && sc.why != Why::Zone && sc.why != Why::Body)
    return 0;
  return sc.stopAt <= a.claim.s + kAtStop || sc.inTheWay ? sc.by : 0;
}

std::vector<std::vector<Traffic::AgentId>>
Traffic::cycles(const std::map<AgentId, AgentId>& waitsOn)
{
  // (Each waits on at most one: follow each chain; a chain coming back on
  // itself is a cycle, found once, from its lowest id.)
  std::vector<std::vector<AgentId>> out;
  std::set<AgentId> done;
  for (const auto& [start, unused] : waitsOn)
  {
    if (done.contains(start))
      continue;
    std::vector<AgentId> chain;
    std::map<AgentId, std::size_t> at;
    AgentId cur = start;
    while (true)
    {
      if (const auto seen = at.find(cur); seen != at.end())
      {
        std::vector<AgentId> cycle(
            chain.begin() + static_cast<std::ptrdiff_t>(seen->second),
            chain.end());
        const auto lowest = std::min_element(cycle.begin(), cycle.end());
        std::rotate(cycle.begin(), lowest, cycle.end());
        if (cycle.size() >= 2)
          out.push_back(std::move(cycle));
        break;
      }
      if (done.contains(cur))
        break; // (joins a chain already followed)
      at[cur] = chain.size();
      chain.push_back(cur);
      const auto next = waitsOn.find(cur);
      if (next == waitsOn.end() || !next->second)
        break;
      cur = next->second;
    }
    done.insert(chain.begin(), chain.end());
  }
  return out;
}

std::vector<Footprint> Traffic::claimed(AgentId id) const
{
  std::vector<Footprint> out;
  const auto it = m_agents.find(id);
  if (it == m_agents.end())
    return out;
  const Claim& c = it->second.claim;
  out = c.body;
  for (const Sample& t : c.ahead)
    out.insert(out.end(), t.boxes.begin(), t.boxes.end());
  out.insert(out.end(), c.way.begin(), c.way.end());
  return out;
}

Traffic::Schedule Traffic::scheduleOf(AgentId id) const
{
  const auto it = m_agents.find(id);
  return it == m_agents.end() ? Schedule{} : it->second.schedule;
}

std::vector<Footprint>
Traffic::heldBesides(AgentId id, const std::vector<Footprint>& in) const
{
  std::vector<Footprint> out;
  for (const auto& [oid, o] : m_agents)
    if (oid != id && !cooperating(id, oid))
      for (const auto& area : o.held)
        if (in.empty() || !hits(in, area))
          out.insert(out.end(), area.begin(), area.end());
  return out;
}

bool Traffic::holdsZone(AgentId id) const
{
  const auto it = m_agents.find(id);
  return it != m_agents.end() && !it->second.held.empty();
}

} // namespace sfs::nav
