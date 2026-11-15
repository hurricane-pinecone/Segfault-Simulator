#pragma once

#include <cstdint>

namespace sfs::nav
{

/** Where a plan is (every agent kind's). */
enum class PlanState
{
  Pending, // not in yet (on the planning scheduler)
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
  // (Where the time went, diagnostics only: the heuristic built, analytic
  // shots, the coarse graph's corridor, the final check and profile, setup.)
  double heuristicMs = 0.0, shotMs = 0.0, graphMs = 0.0, finishMs = 0.0,
         setupMs = 0.0;
  // (Finer: the heading-aware heuristic (part of heuristicMs); finishMs's
  // parts: the smoother, the final exact check, the speed profile.)
  double headingMs = 0.0, smoothMs = 0.0, exactMs = 0.0, profileMs = 0.0;
  // (Its collision queries (nav::checkStats()): answered exactly, settled by
  // the broad phase, and the milliseconds in them all.)
  uint64_t checks = 0, broadChecks = 0;
  double checkMs = 0.0;
  // (The steering words worked out for shots (part of shotMs), the
  // near-goal words of the heuristic, and the cost layer's prices: asked,
  // and their milliseconds.)
  double wordsMs = 0.0, nearMs = 0.0, costMs = 0.0;
  uint64_t costCalls = 0;
  // (How it got to its way: the ways found (each quicker than the last), the
  // first one's time (seconds, as the search prices it) and the poses grown
  // when it and the one taken were found.)
  uint32_t ways = 0, firstAt = 0, bestAt = 0;
  float firstSeconds = 0.0f, bestSeconds = 0.0f;
};

} // namespace sfs::nav
