#pragma once

#include <cstdint>

namespace sfs::nav
{

/** Where a plan is (every agent kind's). */
enum class PlanState
{
  Pending, // not looked at yet (Planner)
  Partial, // out of budget: `path` is the clear way so far; it searches on
  Found,
  Blocked, // proven: no way exists (the search exhausted every pose)
};

struct PlanStats
{
  uint32_t nodes = 0;     // poses grown
  uint32_t shots = 0;     // analytic shots tried
  int rounds = 0;         // budgets (1, 2, 4.. times the first)
  int widened = 0;        // times its area (or corridor) was widened
  bool plain = false;     // the start's own shot was the quickest way
  bool exactMiss = false; // the final exact check found a touch (a bug)
  double ms = 0.0;        // wall clock (diagnostics only, never a limit)
};

} // namespace sfs::nav
