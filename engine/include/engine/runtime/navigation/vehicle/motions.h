#pragma once

#include "engine/runtime/navigation/costLayer.h"
#include "engine/runtime/navigation/space.h"
#include "engine/runtime/navigation/vehicle/planner.h"
#include "engine/runtime/navigation/vehicle/steering.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace sfs::nav::vehicle
{

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr float kUnknown = 1.0e5f; // (a cell the 2D heuristic can't reach)

// The curvature levels the paths are laid at: full lock and wider (a wide
// bend at a higher speed is often quicker), each with the sharpness that
// lets the wheel keep up at that bend's own speed.
constexpr float kLevels[] = {1.0f, 0.5f, 0.25f};

inline float bendSpeed(const VehicleModel& v, float k)
{
  return v.speedAt(1, std::max(k, 1e-6f), 0.0f);
}

// The cost layer's price at a pose, counted and timed into this thread's
// CheckStats (diagnostics: how much of a plan the ground's costs take). One
// call in 8 is timed and counted for 8 (reading the clock twice a call cost
// about as much as a cheap price).
inline float priceAt(const CostLayer& cost, Pose2 at, int mode)
{
  CheckStats& stats = checkStats();
  if ((stats.costs++ & 7u) != 0)
    return cost.factor(at, mode);
  const auto t0 = std::chrono::steady_clock::now();
  const float f = cost.factor(at, mode);
  stats.costMs += 8.0 * std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
  return f;
}

// The vehicle in the mode it's driven in, its probes, and the one walk along
// segments that checks and prices them (the search's, the smoother's, the
// final check's alike).
class Motions
{
public:
  const PlanRequest& q;
  const Space& space;
  const CostLayer* cost;
  std::vector<Box3> boxes;
  float minRadius;
  bool forwardOnly;
  // Probes by how far a pose stands for the motion either side of it:
  // stretched (kExt) and widened (kWide) to cover it, made as they're needed.
  static constexpr int kExts = 10, kWides = 9;
  static constexpr float kExt[kExts] =
      {0.0f, 0.05f, 0.1f, 0.15f, 0.2f, 0.3f, 0.45f, 0.65f, 0.9f, 1.2f};
  static constexpr float kWide[kWides] =
      {0.0f, 0.01f, 0.02f, 0.04f, 0.07f, 0.1f, 0.14f, 0.2f, 0.3f};
  mutable std::vector<std::shared_ptr<const void>> body, tow;
  float grow = 0.0f;
  float reachX = 0.0f, reachY = 0.0f; // (the boxes' furthest corner)
  float towX = 0.0f, towY = 0.0f;     // (the trailer's, from its axle)
  std::vector<Steering> levels;
  float inscribed = 0.0f;
  // (The rig's length, nose to tail, its trailer straight behind: how far a
  // goal asking for a gear is come to in it (Goal::arriveGear).)
  float length = 0.0f;
  // (walk()'s scratch: a Motions serves one thread.)
  mutable std::vector<Pose2> m_poses;
  mutable std::vector<float> m_hitches;

  // `search`: the steering levels a search lays its motions with are made
  // too (milliseconds when a vehicle's numbers are new); a check (touches(),
  // touchesAlong(), priceOf(): walk() only) needs none.
  explicit Motions(const PlanRequest& r, bool search = true)
      : q(r), space(*r.space), cost(r.cost.get())
  {
    const VehicleModel& v = q.vehicle;
    if (q.mode > 0 && q.mode <= static_cast<int>(v.modes.size()))
    {
      const VehicleModel::Mode& m =
          v.modes[static_cast<std::size_t>(q.mode - 1)];
      boxes = m.boxes.empty() ? v.boxes : m.boxes;
      minRadius = m.minRadius;
      forwardOnly = m.forwardOnly || !v.mayReverse;
    }
    else
    {
      boxes = v.boxes;
      minRadius = v.minRadius;
      forwardOnly = !v.mayReverse;
    }
    const float kmax = 1.0f / std::max(minRadius, 0.01f);
    for (const Box3& b : boxes)
    {
      reachX = std::max({reachX, -b.back, b.front});
      reachY = std::max(reachY, b.half + std::abs(b.shift));
    }
    if (v.trailer)
    {
      towX = std::max(-v.trailer->box.back, v.trailer->box.front);
      towY = v.trailer->box.half + std::abs(v.trailer->box.shift);
    }
    {
      float nose = 0.0f, tail = 0.0f;
      for (const Box3& b : boxes)
      {
        nose = std::max(nose, b.front);
        tail = std::max(tail, -b.back);
      }
      if (v.trailer)
        tail = std::max(tail,
                        v.trailer->hitchBehind + v.trailer->drawbar -
                            std::min(v.trailer->box.back, 0.0f));
      length = nose + tail;
    }
    grow = q.tunables.gap;
    body.resize(kExts * kWides);
    tow.resize(kExts * kWides);
    for (float level : kLevels)
    {
      if (!search)
        break;
      const float k = kmax * level;
      levels.emplace_back(k, v.curvatureRate() / bendSpeed(v, k));
    }
    // (How near the reference point may come to something: the 2D
    // heuristic's walls are pushed out by this, never more.)
    inscribed = 1e9f;
    for (const Box3& b : boxes)
    {
      if (b.back > 0.0f || b.front < 0.0f || std::abs(b.shift) > b.half)
        continue;
      inscribed =
          std::min({inscribed, -b.back, b.front, b.half - std::abs(b.shift)});
    }
    if (inscribed > 1e8f)
      inscribed = 0.0f;
  }
  // The probe standing for motion `ext` either way along, `wide` sideways
  // (nullptr: more than the biggest: finer steps needed).
  const void* probe(bool trailer, float ext, float wide) const
  {
    int e = 0;
    while (e < kExts && kExt[e] < ext - 1e-5f)
      ++e;
    int w = 0;
    while (w < kWides && kWide[w] < wide - 1e-5f)
      ++w;
    if (e >= kExts || w >= kWides)
      return nullptr;
    auto& slot =
        (trailer ? tow : body)[static_cast<std::size_t>(e * kWides + w)];
    if (!slot)
      slot = trailer ? space.prepare(std::span(&q.vehicle.trailer->box, 1),
                                     grow,
                                     kExt[e],
                                     kWide[w])
                     : space.prepare(boxes, grow, kExt[e], kWide[w]);
    return slot.get();
  }
  // Whether the rig standing for the motion from `a` to `b` (hitch ha, hb)
  // touches nothing: one look covering it, else finer looks between.
  bool clearBetween(Pose2 a,
                    Pose2 b,
                    float ha,
                    float hb,
                    float k,
                    int depth = 0) const
  {
    // One look, at the middle, its probe covering the whole motion: stretched
    // half the step (and the corners' swing along), widened by the corners'
    // swing across and the arc's bow off the chord.
    const float ds = glm::length(b.position - a.position);
    const float turn = std::abs(wrapAngle(b.heading - a.heading));
    const Pose2 m{0.5f * (a.position + b.position),
                  a.heading + 0.5f * wrapAngle(b.heading - a.heading)};
    const float hm = ha + 0.5f * wrapAngle(hb - ha);
    const float bow = 0.125f * ds * ds * std::abs(k);
    const void* pb = probe(
        false, 0.5f * ds + 0.5f * turn * reachY, 0.5f * turn * reachX + bow);
    bool ok = pb && space.fits(pb, m);
    if (ok && q.vehicle.trailer)
    {
      const Pose2 ta = trailerPose(*q.vehicle.trailer, a, ha);
      const Pose2 tb = trailerPose(*q.vehicle.trailer, b, hb);
      const Pose2 tm = trailerPose(*q.vehicle.trailer, m, hm);
      const float tds = glm::length(tb.position - ta.position);
      const float tturn = std::abs(wrapAngle(tb.heading - ta.heading));
      const void* pt = probe(true,
                             0.5f * tds + 0.5f * tturn * towY,
                             0.5f * tturn * towX + 0.125f * tds * tturn);
      ok = pt && space.fits(pt, tm);
    }
    if (ok)
      return true;
    // (Too much motion for one look, or a look that touched: in halves, down
    // to a couple of centimetres (then it's a touch).)
    if (depth >= 4 || ds < 0.02f)
      return false;
    return clearBetween(a, m, ha, hm, k, depth + 1) &&
           clearBetween(m, b, hm, hb, k, depth + 1);
  }
  // Whether the samples i0..i1 of a stretch are clear: one look covering
  // them all (open ground: the usual answer), else in halves.
  bool clearRange(const std::vector<Pose2>& p,
                  const std::vector<float>& h,
                  float kmax,
                  float ds,
                  std::size_t i0,
                  std::size_t i1) const
  {
    if (i1 - i0 <= 1)
      return clearBetween(p[i0], p[i1], h[i0], h[i1], kmax);
    const std::size_t m = (i0 + i1) / 2;
    const float half = static_cast<float>(std::max(m - i0, i1 - m)) * ds;
    const float turn =
        std::max(std::abs(wrapAngle(p[m].heading - p[i0].heading)),
                 std::abs(wrapAngle(p[i1].heading - p[m].heading)));
    const void* pb = probe(
        false, half + turn * reachY, turn * reachX + 0.5f * kmax * half * half);
    bool ok = pb && space.fits(pb, p[m]);
    if (ok && q.vehicle.trailer)
    {
      const Trailer& t = *q.vehicle.trailer;
      const Pose2 ta = trailerPose(t, p[i0], h[i0]),
                  tm = trailerPose(t, p[m], h[m]),
                  tb = trailerPose(t, p[i1], h[i1]);
      const float th = std::max(glm::length(tm.position - ta.position),
                                glm::length(tb.position - tm.position));
      const float tt = std::max(std::abs(wrapAngle(tm.heading - ta.heading)),
                                std::abs(wrapAngle(tb.heading - tm.heading)));
      const void* pt = probe(true, th + tt * towY, tt * towX + 0.5f * th * tt);
      ok = pt && space.fits(pt, tm);
    }
    if (ok)
      return true;
    return clearRange(p, h, kmax, ds, i0, m) &&
           clearRange(p, h, kmax, ds, m, i1);
  }
  float speedFor(int gear, float k, float dkds) const
  {
    return std::max(
        std::min(q.vehicle.speedAt(gear, k, dkds), q.speedCap), 0.1f);
  }

  struct Walk
  {
    float cost = 0.0f;
    float hitch = 0.0f;
    int gear = 0;        // the last gear (0: none yet)
    float escape = 0.0f; // metres left to get out of a touch
    Pose2 end;
    std::optional<Space::Hit> hit; // (if asked: what it touched)
    // (Radians a trailer may pass its limits: the search keeps a little short,
    // its poses being further apart than the final check's.)
    float slack = -0.03f;
  };
  bool failed(Walk& w, Pose2 at, bool wantHit) const
  {
    w.end = at; // (where it failed)
    if (wantHit)
    {
      w.hit = space.hit(probe(false, 0.0f, 0.0f), at);
      if (!w.hit && q.vehicle.trailer)
        w.hit = space.hit(probe(true, 0.0f, 0.0f),
                          trailerPose(*q.vehicle.trailer, at, w.hitch));
    }
    return false;
  }
  // Along `segs` from `from`: false if it touches (a pose a `step` apart,
  // the boxes stretched over the motion between) or the trailer passes its
  // limits; prices it on the way (seconds, `w.cost` added to).
  bool walk(Pose2 from,
            std::span<const Segment> segs,
            float step,
            bool check,
            Walk& w,
            bool wantHit = false) const
  {
    const VehicleModel& v = q.vehicle;
    const Tunables& t = q.tunables;
    Pose2 at = from;
    for (const Segment& seg : segs)
    {
      if (seg.length <= 0.0f)
        continue;
      if (forwardOnly && seg.gear < 0)
        return false;
      if (w.gear != 0 && seg.gear != w.gear)
        w.cost += v.cuspSeconds(t.turnSpeed);
      // (From a stand: the time pulling away, as the speed profile has it.)
      if (w.gear == 0)
        w.cost += v.pullAwaySeconds(
            speedFor(seg.gear, seg.k0, (seg.k1 - seg.k0) / seg.length));
      w.gear = seg.gear;
      const int n =
          std::max(1, static_cast<int>(std::ceil(seg.length / step - 1e-4f)));
      const float ds = seg.length / static_cast<float>(n);
      const float dkds = (seg.k1 - seg.k0) / seg.length;
      // (Clear of a touch already: the stretch's samples gathered and
      // looked over at once (clearRange); getting out of one: pose by pose.)
      const bool together = check && w.escape <= 0.0f;
      // (Priced once it's found clear, in the same order: a way that touches
      // never asks the cost layer. Where it touches is asked for: as it goes.)
      const bool defer = together && !wantHit && cost;
      std::vector<Pose2>& poses = m_poses;
      std::vector<float>& hitches = m_hitches;
      poses.clear();
      hitches.clear();
      if (together)
      {
        poses.push_back(at);
        hitches.push_back(w.hitch);
      }
      for (int i = 0; i < n; ++i)
      {
        const float k0 = seg.k0 + dkds * ds * static_cast<float>(i);
        const float k1 = k0 + dkds * ds;
        const float km = 0.5f * (k0 + k1);
        const Pose2 was = at;
        const float hitchWas = w.hitch;
        at = advance(at, {seg.gear, ds, k0, k1}, ds);
        if (!defer)
        {
          float factor = 1.0f;
          if (cost)
          {
            factor = priceAt(*cost, at, q.mode);
            if (!std::isfinite(factor))
              return false;
          }
          w.cost += ds / speedFor(seg.gear, km, dkds) * factor;
        }
        if (v.trailer)
        {
          w.hitch = hitchStep(
              *v.trailer, w.hitch, static_cast<float>(seg.gear) * ds, km);
          const float limit =
              seg.gear > 0 ? v.trailer->forwardLimit : v.trailer->reverseLimit;
          if (std::abs(w.hitch) > limit + w.slack &&
              std::abs(w.hitch) > std::abs(hitchWas))
            return false;
        }
        if (together)
        {
          poses.push_back(at);
          hitches.push_back(w.hitch);
          continue;
        }
        if (!check)
          continue;
        if (clearBetween(was, at, hitchWas, w.hitch, km))
          w.escape = 0.0f; // (clear once, it stays clear)
        else if (w.escape > 0.0f)
          w.escape -= ds;
        else
          return failed(w, at, wantHit);
      }
      if (together && !clearRange(poses,
                                  hitches,
                                  std::max(std::abs(seg.k0), std::abs(seg.k1)),
                                  ds,
                                  0,
                                  poses.size() - 1))
      {
        // (Where: the first sample that touches, for the debug marker.)
        if (wantHit)
          for (std::size_t i = 1; i < poses.size(); ++i)
            if (!clearBetween(poses[i - 1],
                              poses[i],
                              hitches[i - 1],
                              hitches[i],
                              std::max(std::abs(seg.k0), std::abs(seg.k1))))
              return failed(w, poses[i], true);
        return false;
      }
      if (defer)
        for (int i = 0; i < n; ++i)
        {
          const float k0 = seg.k0 + dkds * ds * static_cast<float>(i);
          const float k1 = k0 + dkds * ds;
          const float km = 0.5f * (k0 + k1);
          const float factor =
              priceAt(*cost, poses[static_cast<std::size_t>(i) + 1], q.mode);
          if (!std::isfinite(factor))
            return false;
          w.cost += ds / speedFor(seg.gear, km, dkds) * factor;
        }
    }
    w.end = at;
    return true;
  }
  // THE stand-here check (touches()): a plan's start, its goals, and a
  // game's own "can it stop here" alike.
  std::optional<Space::Hit> touches(Pose2 p, std::optional<float> hitch) const
  {
    if (std::optional<Space::Hit> hit = space.hit(probe(false, 0.0f, 0.0f), p))
      return hit;
    if (!q.vehicle.trailer || !hitch)
      return std::nullopt;
    return space.hit(
        probe(true, 0.0f, 0.0f), trailerPose(*q.vehicle.trailer, p, *hitch));
  }
  bool standsClear(Pose2 p, float hitch) const { return !touches(p, hitch); }
  // At least the time walk() can price a word at, ground and all: each
  // segment at the fastest it may go anywhere along it (its least curvature),
  // on the cheapest ground (floor()); no cusps or pulling away counted. A
  // shot that can't beat the best way found by this isn't walked.
  float leastTime(std::span<const Segment> segs) const
  {
    float c = 0.0f;
    for (const Segment& s : segs)
    {
      if (s.length <= 0.0f)
        continue;
      const float dkds = (s.k1 - s.k0) / s.length;
      const float k =
          s.k0 * s.k1 <= 0.0f ? 0.0f : std::min(std::abs(s.k0), std::abs(s.k1));
      c += s.length / speedFor(s.gear, k, dkds);
    }
    return c * (cost ? cost->floor() : 1.0f);
  }
  // At least the time any way between two points takes (straight, flat out
  // in its faster gear, on the cheapest ground).
  float leastTime(glm::vec2 a, glm::vec2 b) const
  {
    const float top =
        std::max(speedFor(1, 0.0f, 0.0f), speedFor(-1, 0.0f, 0.0f));
    return glm::length(b - a) / top * (cost ? cost->floor() : 1.0f);
  }
  // A rough time for a word (no ground sampled): for sorting candidates.
  float estimate(std::span<const Segment> segs) const
  {
    float c = 0.0f;
    int gear = 0;
    for (const Segment& s : segs)
    {
      const float dkds = s.length > 0 ? (s.k1 - s.k0) / s.length : 0.0f;
      float dt =
          s.length /
          speedFor(s.gear, 0.5f * (std::abs(s.k0) + std::abs(s.k1)), dkds);
      if (gear != 0 && gear != s.gear)
        c += q.vehicle.cuspSeconds(q.tunables.turnSpeed);
      gear = s.gear;
      c += dt;
    }
    return c * (cost ? cost->floor() : 1.0f);
  }
  // Words laid out flat (no allocation per word once its buffers have
  // grown): word i is at(i), cheapest first.
  struct WordList
  {
    std::vector<Segment> segs;
    struct Entry
    {
      float estimate;
      uint32_t first, count;
    };
    std::vector<Entry> order;
    std::size_t size() const { return order.size(); }
    std::span<const Segment> at(std::size_t i) const
    {
      return {segs.data() + order[i].first, order[i].count};
    }
  };
  // Every word from a to b at every curvature level, cheapest first (with
  // cusps: the three-point turns and `cuspSamples` C|CSC / CSC|C first
  // turns each way, at full lock), into `out`.
  void
  wordsInto(Pose2 a, Pose2 b, bool cusps, int cuspSamples, WordList& out) const
  {
    out.segs.clear();
    out.order.clear();
    for (std::size_t i = 0; i < levels.size(); ++i)
    {
      Steering::Words w;
      w.reverse = !forwardOnly;
      w.cusps = cusps && !forwardOnly && i == 0;
      w.cuspSamples = cuspSamples;
      levels[i].paths(
          a,
          b,
          w,
          [&](std::span<const Segment> p)
          {
            out.order.push_back({estimate(p),
                                 static_cast<uint32_t>(out.segs.size()),
                                 static_cast<uint32_t>(p.size())});
            out.segs.insert(out.segs.end(), p.begin(), p.end());
          });
    }
    std::stable_sort(out.order.begin(),
                     out.order.end(),
                     [](const WordList::Entry& x, const WordList::Entry& y)
                     { return x.estimate < y.estimate; });
  }
  // The same as vectors.
  std::vector<std::vector<Segment>>
  words(Pose2 a, Pose2 b, bool cusps, int cuspSamples = 10) const
  {
    WordList list;
    wordsInto(a, b, cusps, cuspSamples, list);
    std::vector<std::vector<Segment>> out;
    out.reserve(list.size());
    for (std::size_t i = 0; i < list.size(); ++i)
      out.emplace_back(list.at(i).begin(), list.at(i).end());
    return out;
  }
};

// Whether a word really got there (a degenerate one may not: dropped).
inline bool arrives(Pose2 end, Pose2 target)
{
  return glm::length(end.position - target.position) < 0.01f &&
         std::abs(wrapAngle(end.heading - target.heading)) < 0.005f;
}

} // namespace sfs::nav::vehicle
