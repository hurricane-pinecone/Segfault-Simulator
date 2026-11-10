#pragma once

#include "engine/runtime/navigation/geometry.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace sfs::nav
{

/** An upright box on the ground (2D): its middle, half sizes, heading. */
struct Footprint
{
  glm::vec2 centre{0.0f};
  glm::vec2 half{0.5f};
  float heading = 0.0f;
};
/** Whether two footprints come within `margin` of each other (either way
 * round the same). */
bool overlaps(const Footprint& a, const Footprint& b, float margin = 0.0f);
/** Whether any of `a` comes within `margin` of any of `b`. */
bool hits(const std::vector<Footprint>& a,
          const std::vector<Footprint>& b,
          float margin = 0.0f);

/**
 * Multi-agent traffic, the textbook way (prioritised planning with a
 * space-time reservation table, as AGV and mining fleets run; Erdmann &
 * Lozano-Perez 1987, Silver 2005), any kind of agent:
 *
 * - **Fixed priorities.** Agents are ranked by their priority (from their
 *   task: a working machine first), then by id: one total order, never
 *   negotiated per meeting, so a pair never swap who goes first.
 * - **Reservations.** In that order, each agent reserves the ground its body
 *   sweeps along its path, per time window (its speed profile's), from where
 *   it is to where it will stand; its body includes its stopping stretch.
 *   A lower agent keeps out of every higher reservation: where its way meets
 *   one in space and time it stands at the last point of its path where its
 *   body is clear of the higher agents' reservations (that one's whole way
 *   on included) and that isn't a no-wait place (CostLayer::noWait). With
 *   nowhere clear on its path, or standing in the way of a higher agent
 *   that stands for its body, it is `inTheWay`: its fleet plans it to a
 *   wait spot clear of them (reservedAbove()).
 * - **Zones.** Narrow places (where two agents can't pass: Claim::zones,
 *   found from the statics) are mutexes: an agent's narrow stretch is
 *   granted, all of it at once, only while no other agent holds ground it
 *   overlaps (granted in rank order); nobody enters ground another holds
 *   until it has left it. Never hold-and-wait: a stretch it isn't in yet is
 *   given up while it stands short of it (for another's reservation, body
 *   or zone). One in a narrow place a higher agent waits for goes first:
 *   that one's reservation doesn't hold it, only its body does (then it
 *   is `inTheWay`).
 * - **Permanent reservations.** An agent marked solid (nothing to do,
 *   parked, stuck) is a static obstacle: a path into it is re-planned round
 *   it (Why::Solid), never waited on.
 *
 * Nobody drives on into another's body (Why::Body), whatever its rank. An
 * agent standing short of another's body reserves only up to there, so one
 * already in front of it (crossing its way) carries on through.
 *
 * - **Getting out of the way.** A lower agent already in a higher one's
 *   way (its body in the ground that one claims, as planned) that is on
 *   its way out of it (moving, or leaving it for a wait spot) is never
 *   held there by that one's reservation: standing there it would only be
 *   in its way. It goes on out, and the higher one stands short of its way
 *   out where they'd meet in time (wayOut()), as it stands for a body.
 * - **Easing off.** A lower agent whose stop is short of a crossing that
 *   the higher one will have cleared in time (its reservation there ends,
 *   it won't stand in it) slows to arrive just as it clears
 *   (Schedule::easeTo) instead of braking to a stand at its stop; it still
 *   never passes the stop while the crossing is reserved.
 * - **Deadlocks.** Who stands waiting for whom is a wait-for graph
 *   (waitsOn(): each agent waits on at most one); a cycle in it
 *   (cycles()) is a deadlock nobody leaves by waiting. Fleet breaks it.
 */
class Traffic
{
public:
  using AgentId = uint64_t;
  struct Sample
  {
    float s = 0.0f;                  // metres along its path
    float enter = 0.0f, exit = 0.0f; // seconds from now it's there, gone
    std::vector<Footprint> boxes;
  };
  /** A narrow stretch of its way: two can't pass there. */
  struct Zone
  {
    float from = 0.0f, to = 0.0f; // metres along its path
    std::vector<Footprint> area;  // the ground its body covers along it
  };
  struct Claim
  {
    int priority = 0;
    // Where it is now; moving, its stopping stretch too (the way it goes on
    // before it can stand, braking now): it's committed to that.
    std::vector<Footprint> body;
    // How many of `body`'s first boxes are where it is now (the rest its
    // stopping stretch); 0: all of them.
    std::size_t here = 0;
    float s = 0.0f;        // how far along its path it is
    bool standing = false; // no path to drive (its body is its claim)
    bool solid = false;    // a static obstacle (Fleet::solid()): permanent
    // Not moving (slower than kStill): its body is passed at the planners'
    // own gap, as one standing (planners plan past it at that gap).
    bool still = false;
    std::vector<Sample> ahead; // its path's next seconds
    // The rest of its way past `ahead` (no times; its end too): where one
    // lower stands clear of.
    std::vector<Footprint> way;
    std::vector<Zone> zones; // narrow stretches on its way, in order
    // On its way to a wait spot off this one's way: its held ground doesn't
    // stop it (it's leaving; bodies still do).
    AgentId leaving = 0;
    // Breaking a deadlock on its way to a wait spot: nobody's held ground
    // stops it (bodies still do).
    bool leavingAll = false;
  };

  /** Each frame, each agent: where it is and what it claims... */
  void update(AgentId id, Claim claim);
  /** ...then every agent's schedule, in rank order. */
  void schedule();
  void remove(AgentId id);
  /** A pair that keep their own spacing (a cart alongside its combine). */
  void cooperate(AgentId a, AgentId b, bool on);
  void setNoWait(std::function<bool(glm::vec2)> noWait)
  {
    m_noWait = std::move(noWait);
  }
  void setMargin(float metres) { m_margin = metres; }
  float margin() const { return m_margin; }

  /** What an agent is to do this frame. */
  struct Schedule
  {
    enum class Why
    {
      None,     // on its way
      Reserved, // keeps out of a higher agent's reservation
      Zone,     // a narrow place another holds
      Body,     // another's body on its way
      Solid,    // a static obstacle on its way: plan round it
    };
    Why why = Why::None;
    float stopAt = std::numeric_limits<float>::infinity(); // m along
    AgentId by = 0; // whose reservation, zone, body or solid
    // Standing in a higher agent's way with nowhere clear on its path:
    // to a wait spot clear of reservedAbove().
    bool inTheWay = false;
    // m/s it eases off to (Reserved, its stop short of a crossing the
    // higher ones will have cleared in time): it gets to its stop as the
    // crossing clears, never braking to a stand there. Infinity: none.
    float easeTo = std::numeric_limits<float>::infinity();
  };
  Schedule scheduleOf(AgentId id) const;
  /** Whether `a` ranks above `b` (goes first). */
  bool ranksAbove(AgentId a, AgentId b) const;
  /**
   * Everything an agent must stand clear of while it waits (a wait spot):
   * every agent ranked above it, its body and all its way on (its next
   * seconds and on to wayReach, past where it stands for now), and the
   * narrow ground others hold.
   */
  std::vector<Footprint> reservedAbove(AgentId id) const;
  /**
   * The narrow ground every agent but `id` holds (never to be entered),
   * leaving out each held stretch `in` (its body) is in already: it goes
   * out through that.
   */
  std::vector<Footprint>
  heldBesides(AgentId id, const std::vector<Footprint>& in = {}) const;
  /** Whether `id` holds a narrow place. */
  bool holdsZone(AgentId id) const;
  bool has(AgentId id) const { return m_agents.contains(id); }
  /**
   * Whom it stands waiting for: held still, short of their reservation,
   * the narrow place they hold or their body (0: nobody; on its way, or
   * slowing only). Its edge in the wait-for graph.
   */
  AgentId waitsOn(AgentId id) const;
  /**
   * The cycles of a wait-for graph (each agent waits on at most one), each
   * once, its members in waiting order from the lowest id: deadlocks.
   */
  static std::vector<std::vector<AgentId>>
  cycles(const std::map<AgentId, AgentId>& waitsOn);
  /**
   * All the ground it claims: its body, its next seconds and its way on (to
   * wayReach): where one yielding to it in a deadlock waits clear of.
   */
  std::vector<Footprint> claimed(AgentId id) const;
  /** Where a way planned for an agent would first be held standing in
   * another's way (crossing()): its sample, and whose way (-1: nowhere). */
  struct Crossing
  {
    int at = -1;
    AgentId by = 0;
  };
  /**
   * A way planned for `id` (`way`: its body where it starts, its samples
   * along all of it, their times from now; leaving/leavingAll as it will
   * claim it) checked as schedule() will check it, against every agent
   * ranked above it, each one's reservation as planned (its body, all its
   * next seconds and its way on, not cut short where it stands for now;
   * one standing for another's body, zone or reservation: up to its stop):
   * the first sample that meets one with no place before it clear of them
   * all to stand and wait in (it would be held in their way). Ground it's
   * on its way out of (wayOut()) doesn't hold it. One with a clear place
   * to wait short of the meeting isn't crossing: it waits there. That's
   * anywhere on its way short of it (`onTheWay`: a trip), or only where it
   * starts (a way to a wait spot: it's off to wait clear of them).
   */
  Crossing crossing(AgentId id, const Claim& way, bool onTheWay = true) const;

private:
  struct Agent
  {
    Claim claim;
    Schedule schedule;
    float reservedTo = std::numeric_limits<float>::infinity(); // (its stop)
    std::vector<std::vector<Footprint>> held; // narrow ground it holds
    // (Each held area's start, m along its path: -infinity, it's in it.)
    std::vector<float> heldFrom;
  };
  bool cooperating(AgentId a, AgentId b) const;
  float bodyMargin(const Claim& c) const;
  // Whether a sample meets `h`'s reservation in space and time (`body`: its
  // body too).
  bool meets(const Sample& s, const Agent& h, bool body = true) const;
  // Standing still touching `o`'s body: its way out let off (kEscapeReach).
  bool escaping(const Claim& c, const Claim& o) const;
  // Whether `boxes` come into the ground `h` claims as planned (its body,
  // all its next seconds, its way on), within the traffic margin.
  bool inGround(const std::vector<Footprint>& boxes, const Claim& h) const;
  // Its body in `h`'s ground, on its way out of it (moving): how many of
  // its next seconds' samples, from its first, take it out (through the
  // first clear of it). Leaving it for a wait spot: through the last of
  // its way there that's in it. 0: not in it, or not on its way out.
  std::size_t wayOut(const Claim& c, AgentId hid, const Claim& h) const;
  // (Its samples meet `h`'s reservation as planned: body, next seconds, way
  // on from when they end.)
  bool meetsPlanned(const Sample& s, const Claim& h) const;
  bool clearOfAbove(AgentId id,
                    const std::vector<Footprint>& boxes,
                    AgentId meeting) const;
  void scheduleOne(AgentId id, Agent& a, const std::vector<AgentId>& above);
  // Seconds till every higher reservation its way meets from `first` on is
  // clear (infinity: one will stand there, or meets it past its next
  // seconds).
  float clearIn(AgentId id,
                const Claim& c,
                std::size_t first,
                const std::vector<AgentId>& above) const;
  void holdZones(AgentId id, Agent& a);
  static void releaseShort(Agent& a, float stopAt);
  std::unordered_map<AgentId, Agent> m_agents;
  // This schedule()'s wayOut() of each (lower, higher) pair met (0: none).
  std::map<std::pair<AgentId, AgentId>, std::size_t> m_wayOut;
  std::set<std::pair<AgentId, AgentId>> m_cooperating;
  std::function<bool(glm::vec2)> m_noWait;
  float m_margin = 0.3f;
};

} // namespace sfs::nav
