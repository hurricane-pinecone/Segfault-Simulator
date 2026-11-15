#pragma once

#include "engine/runtime/navigation/space.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sfs::nav
{

/**
 * THE interface a path search meets, for every agent kind (each kind derives
 * its own from it, adding how it hands back its way: vehicle::ISearch).
 * Searches are added by name to the kind's SearchRegistry and picked by the
 * request (the kind's parameters name it; the vehicle kind's default
 * "hybridAStar"). The stages round it stay the same for every search: the
 * steering (continuous-curvature words), the smoother, the speed profile, the
 * follower. Its contract:
 *
 * - It plans what the kind's request asks over the request's Space and
 *   CostLayer, through the kind's one check of a motion, and hands back the
 *   kind's own types.
 * - Budgets are NODES (whatever its unit of work is), never the clock: run(n)
 *   with the same inputs does the same work and gives the same answer on any
 *   machine at any load. run() again with a bigger budget continues where it
 *   stopped (the same answer as one run with the bigger budget).
 * - Budget: out of budget, bestSoFar() is a clear way from the start (to
 *   drive meanwhile), possibly empty.
 * - Exhausted means PROVEN: every reachable state within its area was tried
 *   and none gets there (leftArea() says whether the area, not the world,
 *   hemmed it in: then it's widened, not proof). A search that cannot prove a
 *   way doesn't exist (sampling, RRT and the like) must answer Unknown, never
 *   Exhausted: "no way" is only ever reported when proven.
 */
class SearchBase
{
public:
  enum class Status
  {
    Found,
    Budget,    // out of budget: run again with more
    Exhausted, // proven: none (within its area)
    Unknown,   // gave up without proof (a search that can't prove)
  };
  virtual ~SearchBase() = default;
  /** Grow up to `budget` nodes in all (counting earlier runs). */
  virtual Status run(uint32_t budget) = 0;
  virtual uint32_t grown() const = 0;
  virtual uint32_t shots() const { return 0; }
  /** Hemmed in by its area (or corridor), not the world: widen, no proof. */
  virtual bool leftArea() const = 0;
  /** Exhausted: what hems it in (for the debug marker). */
  virtual std::vector<Space::Hit> blockers() const { return {}; }
};

/**
 * A kind's searches by name (as RenderBackends): add() offers one, make()
 * builds one for a plan. One per kind (vehicle::searches()).
 */
template <typename Context, typename Search>
class SearchRegistry
{
public:
  using Factory = std::function<std::unique_ptr<Search>(const Context&)>;
  /** Offer a search (replacing any of the same name). */
  void add(std::string name, Factory make)
  {
    std::lock_guard lock(m_mutex);
    for (auto& [n, f] : m_list)
      if (n == name)
      {
        f = std::move(make);
        return;
      }
    m_list.emplace_back(std::move(name), std::move(make));
  }
  /** A new search of that name; null if there's none. */
  std::unique_ptr<Search> make(std::string_view name,
                               const Context& context) const
  {
    Factory f;
    {
      std::lock_guard lock(m_mutex);
      for (const auto& [n, make] : m_list)
        if (n == name)
          f = make;
    }
    return f ? f(context) : nullptr;
  }
  std::vector<std::string> names() const
  {
    std::lock_guard lock(m_mutex);
    std::vector<std::string> out;
    for (const auto& [n, f] : m_list)
      out.push_back(n);
    return out;
  }

private:
  mutable std::mutex m_mutex;
  std::vector<std::pair<std::string, Factory>> m_list;
};

} // namespace sfs::nav
