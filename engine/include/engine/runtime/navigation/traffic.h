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
    float s = 0.0f;            // how far along its path it is
    bool standing = false;     // no path to drive (its body is its claim)
    bool solid = false;        // a static obstacle (Fleet::solid()): permanent
    std::vector<Sample> ahead; // its path's next seconds
    // The rest of its way past `ahead` (no times; its end too): where one
    // lower stands clear of.
    std::vector<Footprint> way;
    std::vector<Zone> zones; // narrow stretches on its way, in order
    // On its way to a wait spot off this one's way: its held ground doesn't
    // stop it (it's leaving; bodies still do).
    AgentId leaving = 0;
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
  };
  Schedule scheduleOf(AgentId id) const;
  /** Whether `a` ranks above `b` (goes first). */
  bool ranksAbove(AgentId a, AgentId b) const;
  /**
   * Everything an agent must stand clear of while it waits: the
   * reservations of every agent ranked above it (their bodies, next
   * seconds and ways on) and the narrow ground others hold.
   */
  std::vector<Footprint> reservedAbove(AgentId id) const;
  /** The narrow ground every agent but `id` holds (never to be entered). */
  std::vector<Footprint> heldBesides(AgentId id) const;
  /** Whether `id` holds a narrow place. */
  bool holdsZone(AgentId id) const;
  bool has(AgentId id) const { return m_agents.contains(id); }

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
  // Whether a sample meets `h`'s reservation in space and time.
  bool meets(const Sample& s, const Agent& h) const;
  bool clearOfAbove(AgentId id,
                    const std::vector<Footprint>& boxes,
                    AgentId meeting) const;
  void scheduleOne(AgentId id, Agent& a, const std::vector<AgentId>& above);
  void holdZones(AgentId id, Agent& a);
  static void releaseShort(Agent& a, float stopAt);
  std::unordered_map<AgentId, Agent> m_agents;
  std::set<std::pair<AgentId, AgentId>> m_cooperating;
  std::function<bool(glm::vec2)> m_noWait;
  float m_margin = 0.3f;
};

} // namespace sfs::nav
