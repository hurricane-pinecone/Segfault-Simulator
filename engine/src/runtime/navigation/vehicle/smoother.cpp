#include "engine/runtime/navigation/vehicle/smoother.h"

namespace sfs::nav::vehicle
{

// Shortcuts: from each pose, the farthest pose further on that a single
// continuous-curvature word reaches clear and no slower than the way there.
std::vector<Segment> smooth(const Motions& rig, const Way& c, const Goal* goal)
{
  std::vector<Segment> out;
  const std::size_t n = c.poses.size();
  if (n < 2)
    return out;
  std::size_t cur = 0;
  Motions::Walk state;
  state.hitch = c.hitch[0];
  state.escape = rig.standsClear(c.poses[0], c.hitch[0]) ? 0.0f : kEscapeReach;
  while (cur + 1 < n)
  {
    bool took = false;
    std::vector<std::size_t> tries;
    for (std::size_t span = n - 1 - cur; span >= 2; span /= 2)
      tries.push_back(cur + span);
    for (const std::size_t j : tries)
    {
      const float chain = c.g[j] - c.g[cur];
      int tried = 0;
      for (const auto& word : rig.words(c.poses[cur], c.poses[j], true))
      {
        if (rig.estimate(word) > chain + 1e-3f || tried++ >= 3)
          break;
        Motions::Walk w = state;
        w.cost = 0.0f;
        if (!rig.walk(c.poses[cur], word, rig.q.tunables.checkStep, true, w) ||
            w.cost > chain + 1e-3f || !arrives(w.end, c.poses[j]))
          continue;
        // (Towing: the trailer must come to pose j as the search had it,
        // or the way on from there isn't the way the search checked.)
        if (rig.q.vehicle.trailer && j < n - 1 &&
            std::abs(wrapAngle(w.hitch - c.hitch[j])) > 0.05f)
          continue;
        if (j == n - 1 && goal && goal->hitch &&
            std::abs(wrapAngle(w.hitch - *goal->hitch)) > goal->hitchTolerance)
          continue;
        out.insert(out.end(), word.begin(), word.end());
        state = w;
        cur = j;
        took = true;
        break;
      }
      if (took)
        break;
    }
    if (took)
      continue;
    // (No shortcut: the way as searched, one link on.)
    const std::vector<Segment>& link = c.links[cur];
    Motions::Walk w = state;
    if (!rig.walk(c.poses[cur], link, rig.q.tunables.checkStep, true, w))
    {
      // (Never: the search's own way from its own state. Should it be, the
      // way as searched, unsmoothed.)
      out.clear();
      for (const auto& l : c.links)
        out.insert(out.end(), l.begin(), l.end());
      return out;
    }
    out.insert(out.end(), link.begin(), link.end());
    state = w;
    ++cur;
  }
  return out;
}

} // namespace sfs::nav::vehicle
