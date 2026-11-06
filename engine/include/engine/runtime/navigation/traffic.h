#pragma once

#include "engine/runtime/navigation/geometry.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
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

/**
 * Right of way between moving agents (any kind): prioritised space-time
 * reservation. Each moving agent claims the ground it will cover over the
 * next few seconds (samples along its path: its footprints there, when it
 * gets there and when it's gone); one standing claims its body for good.
 * Where two claims meet, priority decides who gives way: the agents' own
 * priority (a working machine first), then whoever claimed that stretch
 * first, then the lower id (a total order, kept while the meeting lasts).
 * Only the one giving way acts, and its path never changes for it:
 *
 * - HoldAt: it stops at the last point of its path where its whole body is
 *   clear of the other's claim and not where nothing may wait (gateways,
 *   CostLayer::noWait), and goes on once the other's claim has left its way.
 * - ClearOut: it's already in the other's way (head-on in an aisle): it must
 *   move off it (the caller plans to anywhere clear of `other`'s claim), wait,
 *   then carry on.
 * - Wait: the one with the right of way, when the other's BODY is on its way
 *   (clearing out): it stops, it never re-plans for the other.
 *
 * A yielder that can't clear out (cannotClear()) swaps the order once if the
 * other may give way instead; both unable is a deadlock, reported
 * (deadlocks()), never silent.
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
  struct Claim
  {
    int priority = 0;
    std::vector<Footprint> body; // where it is now
    float s = 0.0f;              // how far along its path it is
    bool standing = false;       // not moving on (its body is its claim)
    // Stood still a while, its path kept (held): a solid in plans, so its
    // body is passed as close as its exact boxes allow (as one standing).
    bool parked = false;
    std::vector<Sample> ahead; // its path's next seconds
    uint64_t path = 0;         // which path (a new one: a new claim)
  };

  /** Each frame, each agent: where it is and what it claims. */
  void update(AgentId id, Claim claim, double now);
  void remove(AgentId id);
  /** A pair that keep their own spacing (a cart alongside its combine). */
  void cooperate(AgentId a, AgentId b, bool on);
  void setNoWait(std::function<bool(glm::vec2)> noWait)
  {
    m_noWait = std::move(noWait);
  }
  void setMargin(float metres) { m_margin = metres; }

  struct Advice
  {
    enum class Kind
    {
      Go,
      HoldAt,   // stop by `s` metres along its path
      ClearOut, // get off `other`'s claim
      Wait,     // stand: `other` is on its way
    };
    Kind kind = Kind::Go;
    float s = std::numeric_limits<float>::infinity();
    AgentId other = 0;
  };
  Advice advise(AgentId id) const;
  /** A yielder told to clear out found no way off the other's claim. */
  void cannotClear(AgentId id, AgentId other);
  /** Whether `p` (a footprint) is clear of everything `other` claims. */
  bool clearOf(AgentId other, const std::vector<Footprint>& boxes) const;
  /** Every box `other` claims now (its body, and its way ahead if moving). */
  std::vector<Footprint> claimed(AgentId other) const;
  float margin() const { return m_margin; }
  /** Whether `id` has a claim. */
  bool has(AgentId id) const { return m_agents.contains(id); }
  /** Pairs neither of which can give way: reported, with where. */
  struct Deadlock
  {
    AgentId a = 0, b = 0;
  };
  const std::vector<Deadlock>& deadlocks() const { return m_deadlocks; }

private:
  struct Agent
  {
    Claim claim;
    double since = 0.0; // when its path's claim began
  };
  // Whether a gives way to b (the order, decided once a meeting begins).
  bool yields(AgentId a, AgentId b) const;
  bool meets(const Agent& a, const Agent& b, std::size_t* first) const;
  float bodyMargin(const Claim& c) const;
  std::unordered_map<AgentId, Agent> m_agents;
  std::set<std::pair<AgentId, AgentId>> m_cooperating;
  mutable std::map<std::pair<AgentId, AgentId>, AgentId>
      m_order; // (pair -> who yields)
  std::set<std::pair<AgentId, AgentId>> m_swapped;
  std::vector<Deadlock> m_deadlocks;
  std::function<bool(glm::vec2)> m_noWait;
  float m_margin = 0.3f;
  double m_now = 0.0;
};

} // namespace sfs::nav
