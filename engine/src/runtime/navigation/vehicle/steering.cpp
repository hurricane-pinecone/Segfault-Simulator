#include "engine/runtime/navigation/vehicle/steering.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <mutex>

namespace sfs::nav::vehicle
{

namespace
{
constexpr float kHalfPi = 0.5f * kPi;
constexpr float kClothoidStep = 0.25f; // m: Gauss pieces along a clothoid

// An angle turned in `sense` from `from` to `to`, in [0, 2 pi) (a hair short
// of a whole turn counts as none).
float turned(float from, float to, int sense)
{
  const float d = wrapPositive(static_cast<float>(sense) * (to - from));
  return d > kTwoPi - 1.0e-4f ? 0.0f : d;
}
float angleOf(glm::vec2 v) { return std::atan2(v.y, v.x); }

// A word being laid: its segments in place (at most a C|CSC's ten), never
// on the heap.
struct Word
{
  std::array<Segment, 12> s;
  std::size_t n = 0;
  void push_back(const Segment& x)
  {
    if (n < s.size())
      s[n++] = x;
  }
  bool empty() const { return n == 0; }
  const Segment& front() const { return s[0]; }
  std::span<const Segment> view() const { return {s.data(), n}; }
};
} // namespace

Pose2 advance(Pose2 from, const Segment& seg, float s)
{
  const float g = static_cast<float>(seg.gear);
  const float dk = seg.k1 - seg.k0;
  if (std::abs(dk) < 1.0e-9f || seg.length <= 0.0f)
  {
    const float k = seg.k0;
    if (std::abs(k) < 1.0e-7f)
      return {from.position + g * s * unit(from.heading), from.heading};
    const float h = from.heading + g * k * s;
    const glm::vec2 d{std::sin(h) - std::sin(from.heading),
                      std::cos(from.heading) - std::cos(h)};
    return {from.position + d / k, h};
  }
  // A clothoid: heading quadratic in s, positions by 3-point Gauss-Legendre
  // over pieces of at most kClothoidStep (exact to well under a millimetre).
  const int n = std::max(1, static_cast<int>(std::ceil(s / kClothoidStep)));
  const float h = s / static_cast<float>(n);
  const float rate = dk / seg.length;
  const auto headingAt = [&](float t)
  { return from.heading + g * (seg.k0 * t + 0.5f * rate * t * t); };
  constexpr float kNode = 0.38729833f; // sqrt(3/5) / 2
  glm::vec2 p = from.position;
  for (int i = 0; i < n; ++i)
  {
    const float m = (static_cast<float>(i) + 0.5f) * h;
    p += g * h *
         ((5.0f / 18.0f) * unit(headingAt(m - kNode * h)) +
          (8.0f / 18.0f) * unit(headingAt(m)) +
          (5.0f / 18.0f) * unit(headingAt(m + kNode * h)));
  }
  return {p, headingAt(s)};
}

Pose2 endOf(Pose2 from, std::span<const Segment> segments)
{
  for (const Segment& seg : segments)
    from = advance(from, seg, seg.length);
  return from;
}

float lengthOf(std::span<const Segment> segments)
{
  float l = 0.0f;
  for (const Segment& seg : segments)
    l += seg.length;
  return l;
}

std::vector<Segment> reversed(std::span<const Segment> segments)
{
  std::vector<Segment> out(segments.rbegin(), segments.rend());
  for (Segment& seg : out)
  {
    seg.gear = -seg.gear;
    std::swap(seg.k0, seg.k1);
  }
  return out;
}

Steering::Steering(float kappa, float sigma)
    : m_kappa(std::max(kappa, 1.0e-4f)), m_sigma(std::max(sigma, 1.0e-4f))
{
  const float lc = m_kappa / m_sigma;
  m_thetaC = 0.5f * m_kappa * lc;
  const Pose2 c = advance({}, {1, lc, 0.0f, m_kappa}, lc);
  // The arc's centre seen from the clothoid's start (+x along, +y left).
  const glm::vec2 omega{c.position.x - std::sin(m_thetaC) / m_kappa,
                        c.position.y + std::cos(m_thetaC) / m_kappa};
  m_r = glm::length(omega);
  m_mu = std::atan2(omega.x, omega.y);
  m_R = 1.0f / m_kappa;
  // Elementary turns (smaller than the clothoids allow): for each deflection
  // up to 2 thetaC, the clothoids' length that ends the turn on the CC
  // circle (bisection, once per kappa and sigma: every planner call makes
  // its rig's steering, and the table costs milliseconds), interpolated by
  // turn().
  static std::mutex cacheMutex;
  static std::map<std::pair<float, float>, std::vector<float>> cache;
  {
    std::lock_guard lock(cacheMutex);
    if (const auto it = cache.find({m_kappa, m_sigma}); it != cache.end())
    {
      m_elementary = it->second;
      return;
    }
  }
  constexpr int kSamples = 64;
  m_elementary.resize(kSamples + 1);
  m_elementary[0] = m_r * std::sin(m_mu);
  for (int n = 1; n <= kSamples; ++n)
  {
    const float d = 2.0f * m_thetaC * static_cast<float>(n) / kSamples;
    const auto miss = [&](float l)
    {
      const float k = d / l;
      const Pose2 a = advance({}, {1, l, 0.0f, k}, l);
      const Pose2 b = advance(a, {1, l, k, 0.0f}, l);
      return glm::length(b.position - omega) - m_r;
    };
    float lo = 1.0e-3f, hi = 2.0f * m_r;
    for (int i = 0; i < 40; ++i)
    {
      const float mid = 0.5f * (lo + hi);
      (miss(mid) < 0.0f) == (miss(lo) < 0.0f) ? lo = mid : hi = mid;
    }
    m_elementary[static_cast<std::size_t>(n)] = 0.5f * (lo + hi);
  }
  std::lock_guard lock(cacheMutex);
  cache.emplace(std::pair{m_kappa, m_sigma}, m_elementary);
}

glm::vec2 Steering::centre(Motion m, int sense, End end) const
{
  // Seen from its centre, a turn's start is at (direction - sense (pi/2 +
  // mu)), its end at (direction - sense (pi/2 - mu)) (the CC circle, radius
  // r), a full-lock end at (direction - sense pi/2) (the arc's, 1/kappa).
  const float rho = end == End::Lock ? m_R : m_r;
  const float nu = end == End::Start ? m_mu : end == End::End ? -m_mu : 0.0f;
  return m.p - rho * unit(m.d - static_cast<float>(sense) * (kHalfPi + nu));
}

bool Steering::turn(float deflection,
                    int sense,
                    int gear,
                    bool ccStart,
                    bool ccEnd,
                    std::vector<Segment>& out) const
{
  return turnInto(deflection, sense, gear, ccStart, ccEnd, out);
}

template <class Out>
bool Steering::turnInto(float deflection,
                        int sense,
                        int gear,
                        bool ccStart,
                        bool ccEnd,
                        Out& out) const
{
  const float need =
      (static_cast<float>(ccStart) + static_cast<float>(ccEnd)) * m_thetaC;
  if (!ccStart && !ccEnd && deflection < 1.0e-5f)
    return true; // (a cusp-to-cusp nothing)
  if (ccStart && ccEnd && deflection < need)
  {
    // Smaller than its clothoids: an elementary turn (two clothoids of a
    // gentler sharpness) laid so it still ends on the CC circle (Fraichard &
    // Scheuer's), down to a straight 2 r sin(mu) long for no turn at all.
    if (deflection < 1.0e-6f)
    {
      out.push_back({gear, 2.0f * m_r * std::sin(m_mu), 0.0f, 0.0f});
      return true;
    }
    // (Each clothoid's length for that turn: from the table laid once.)
    const float x =
        deflection / need * static_cast<float>(m_elementary.size() - 1);
    const std::size_t i =
        std::min(static_cast<std::size_t>(x), m_elementary.size() - 2);
    const float f = x - static_cast<float>(i);
    const float l =
        m_elementary[i] + f * (m_elementary[i + 1] - m_elementary[i]);
    const float k = static_cast<float>(gear * sense) * deflection / l;
    out.push_back({gear, l, 0.0f, k});
    out.push_back({gear, l, k, 0.0f});
    return true;
  }
  if (deflection < need - 1.0e-5f)
    return false;
  const float k = static_cast<float>(gear * sense) * m_kappa;
  const float lc = m_kappa / m_sigma;
  if (ccStart)
    out.push_back({gear, lc, 0.0f, k});
  if (const float arc = (deflection - need) / m_kappa; arc > 1.0e-5f)
    out.push_back({gear, arc, k, k});
  if (ccEnd)
    out.push_back({gear, lc, k, 0.0f});
  return true;
}

void Steering::csc(Motion a,
                   Motion b,
                   int gear,
                   const Each& out,
                   bool ccStart) const
{
  // (Between the circles, the straight's ends: Omega2 - Omega1 is, turned
  // to the straight's way, (L + 2 r sin mu, r cos mu (s2 - s1)).)
  const float side = 2.0f * m_r * std::cos(m_mu);
  const float along = 2.0f * m_r * std::sin(m_mu);
  for (int s1 : {1, -1})
    for (int s2 : {1, -1})
    {
      const glm::vec2 o1 = centre(a, s1, ccStart ? End::Start : End::Lock);
      const glm::vec2 o2 = centre(b, s2, End::End);
      const glm::vec2 v = o2 - o1;
      const float dist = glm::length(v);
      float length = 0.0f, d = 0.0f;
      if (s1 == s2)
      {
        if (dist < 1.0e-6f)
          continue;
        length = dist - along;
        d = angleOf(v);
      }
      else
      {
        if (dist < side)
          continue;
        const float x = std::sqrt(std::max(dist * dist - side * side, 0.0f));
        length = x - along;
        d = angleOf(v) - std::atan2(static_cast<float>(s2) * side, x);
      }
      if (length < -1.0e-5f)
        continue;
      Word w;
      if (!turnInto(turned(a.d, d, s1), s1, gear, ccStart, true, w))
        continue;
      if (length > 1.0e-5f)
        w.push_back({gear, length, 0.0f, 0.0f});
      if (!turnInto(turned(d, b.d, s2), s2, gear, true, true, w))
        continue;
      out(w.view());
    }
}

void Steering::ccc(Motion a, Motion b, int gear, const Each& out) const
{
  // (Two CC turns meeting: their centres 2 r apart.)
  const float l = 2.0f * m_r;
  for (int s : {1, -1})
  {
    const glm::vec2 o1 = centre(a, s, End::Start), o3 = centre(b, s, End::End);
    const glm::vec2 v = o3 - o1;
    const float dist = glm::length(v);
    if (dist < 1.0e-6f || dist > 2.0f * l)
      continue;
    const glm::vec2 mid = 0.5f * (o1 + o3), across{-v.y / dist, v.x / dist};
    const float h = std::sqrt(std::max(l * l - 0.25f * dist * dist, 0.0f));
    for (float side : {1.0f, -1.0f})
    {
      const glm::vec2 om = mid + side * h * across;
      const float sf = static_cast<float>(s);
      const float d1 = angleOf(om - o1) + sf * (kHalfPi - m_mu);
      const float d2 = angleOf(o3 - om) - sf * (kHalfPi - m_mu);
      Word w;
      if (turnInto(turned(a.d, d1, s), s, gear, true, true, w) &&
          turnInto(turned(d1, d2, -s), -s, gear, true, true, w) &&
          turnInto(turned(d2, b.d, s), s, gear, true, true, w))
        out(w.view());
    }
  }
}

void Steering::threePoint(Motion a, Motion b, int gear, const Each& out) const
{
  const float l = 2.0f * m_R;
  for (int s : {1, -1})
  {
    const float sf = static_cast<float>(s);
    const glm::vec2 o1 = centre(a, s, End::Start), o3 = centre(b, s, End::End);
    const glm::vec2 v = o3 - o1;
    const float dist = glm::length(v);
    if (dist < 1.0e-6f || dist > 2.0f * l)
      continue;
    const glm::vec2 mid = 0.5f * (o1 + o3), across{-v.y / dist, v.x / dist};
    const float h = std::sqrt(std::max(l * l - 0.25f * dist * dist, 0.0f));
    for (float side : {1.0f, -1.0f})
    {
      const glm::vec2 o2 = mid + side * h * across;
      const float d1 = angleOf(o2 - o1) + sf * kHalfPi;
      const float d2 = angleOf(o3 - o2) + sf * kHalfPi;
      Word w;
      if (turnInto(turned(a.d, d1, s), s, gear, true, false, w) &&
          turnInto(turned(d1 + kPi, d2, s), s, -gear, false, false, w) &&
          turnInto(turned(d2 + kPi, b.d, s), s, gear, false, true, w))
        out(w.view());
    }
  }
}

void Steering::cuspFirst(Motion a,
                         Motion b,
                         int gear,
                         int samples,
                         const Each& out) const
{
  for (int s : {1, -1})
  {
    const float sf = static_cast<float>(s);
    const glm::vec2 o1 = centre(a, s, End::Start);
    for (int i = 0; i < samples; ++i)
    {
      const float deflection =
          m_thetaC + (kTwoPi - m_thetaC) * static_cast<float>(i) /
                         static_cast<float>(std::max(samples, 1));
      const float d1 = a.d + sf * deflection;
      const glm::vec2 p = o1 + m_R * unit(d1 - sf * kHalfPi);
      Word first;
      if (!turnInto(deflection, s, gear, true, false, first))
        continue;
      // (On from the cusp backing the other way, the same motion sense: a
      // turn the other way would only go back over the first.)
      csc(
          {p, d1 + kPi},
          b,
          -gear,
          [&](std::span<const Segment> r)
          {
            if (r.empty() || r.front().gear != -gear ||
                (r.front().k0 * static_cast<float>(-gear) * sf) <= 0.0f)
              return;
            Word w = first;
            for (const Segment& x : r)
              w.push_back(x);
            out(w.view());
          },
          false);
    }
  }
}

std::vector<std::vector<Segment>>
Steering::paths(Pose2 from, Pose2 to, const Words& words) const
{
  std::vector<std::vector<Segment>> out;
  paths(from,
        to,
        words,
        [&](std::span<const Segment> w)
        { out.emplace_back(w.begin(), w.end()); });
  return out;
}

void Steering::paths(Pose2 from,
                     Pose2 to,
                     const Words& words,
                     const Each& each) const
{
  // (Each checked, if asked: it really ends at the goal.)
  const Each verified = [&](std::span<const Segment> w)
  {
    const Pose2 e = endOf(from, w);
    if (glm::length(e.position - to.position) > 0.02f ||
        std::abs(wrapAngle(e.heading - to.heading)) > 0.01f)
      return;
    each(w);
  };
  const Each& out = words.verify ? verified : each;
  const auto motion = [](Pose2 p, int gear) {
    return Motion{p.position, gear > 0 ? p.heading : p.heading + kPi};
  };
  for (int gear : {1, -1})
  {
    if ((gear > 0 && !words.forward) || (gear < 0 && !words.reverse))
      continue;
    csc(motion(from, gear), motion(to, gear), gear, out);
    ccc(motion(from, gear), motion(to, gear), gear, out);
  }
  if (words.cusps)
    for (int gear : {1, -1})
    {
      threePoint(motion(from, gear), motion(to, gear), gear, out);
      if (words.cuspSamples <= 0)
        continue;
      cuspFirst(
          motion(from, gear), motion(to, -gear), gear, words.cuspSamples, out);
      // CSC|C: a C|CSC from the goal back to the start, driven the other way.
      cuspFirst(motion(to, gear),
                motion(from, -gear),
                gear,
                words.cuspSamples,
                [&](std::span<const Segment> w)
                {
                  Word r;
                  for (auto it = w.rbegin(); it != w.rend(); ++it)
                    r.push_back({-it->gear, it->length, it->k1, it->k0});
                  out(r.view());
                });
    }
}

float hitchStep(const Trailer& trailer, float hitch, float ds, float k)
{
  // An off-axle hitch: d(angle) = d(turn) (1 + hitchBehind cos(angle) /
  // drawbar) - d(distance) sin(angle) / drawbar (RK4, steps <= 0.35 m).
  const float hb = trailer.hitchBehind;
  const float db = std::max(trailer.drawbar, 0.1f);
  const float turn = ds * k;
  const int steps = std::max(
      1,
      static_cast<int>(
          std::ceil(std::max(std::abs(ds), std::abs(turn) * hb) / 0.35f)));
  const float s = ds / static_cast<float>(steps);
  const float t = turn / static_cast<float>(steps);
  const auto rate = [&](float a)
  { return t * (1.0f + hb * std::cos(a) / db) - s * std::sin(a) / db; };
  for (int i = 0; i < steps; ++i)
  {
    const float k1 = rate(hitch), k2 = rate(hitch + 0.5f * k1);
    const float k3 = rate(hitch + 0.5f * k2), k4 = rate(hitch + k3);
    hitch += (k1 + 2.0f * k2 + 2.0f * k3 + k4) / 6.0f;
  }
  return wrapAngle(hitch);
}

Pose2 trailerPose(const Trailer& trailer, Pose2 pose, float hitch)
{
  const float heading = wrapAngle(pose.heading - hitch);
  const glm::vec2 at = pose.position - unit(pose.heading) * trailer.hitchBehind;
  return {at - unit(heading) * trailer.drawbar, heading};
}

void sample(VehiclePath& path, float step, const Trailer* trailer, int leg)
{
  path.points.clear();
  Pose2 at = path.start;
  float s = 0.0f, hitch = path.startHitch;
  const auto push = [&](Pose2 p, float k, int gear)
  {
    PathPoint pt;
    pt.s = s;
    pt.pose = p;
    pt.curvature = k;
    pt.gear = gear;
    pt.hitch = hitch;
    pt.leg = leg;
    path.points.push_back(pt);
  };
  if (path.segments.empty())
  {
    push(at, 0.0f, 1);
    return;
  }
  push(at, path.segments.front().k0, path.segments.front().gear);
  for (const Segment& seg : path.segments)
  {
    if (seg.length <= 0.0f)
      continue;
    // (A cusp: the same place twice, the second in the new gear and its
    // wheel turned standing.)
    if (seg.gear != path.points.back().gear)
      push(at, seg.k0, seg.gear);
    const int n = std::max(1, static_cast<int>(std::ceil(seg.length / step)));
    float done = 0.0f;
    for (int i = 1; i <= n; ++i)
    {
      const float t =
          seg.length * static_cast<float>(i) / static_cast<float>(n);
      const float k =
          seg.k0 + (seg.k1 - seg.k0) * (t / std::max(seg.length, 1.0e-6f));
      const float mid =
          seg.k0 + (seg.k1 - seg.k0) *
                       (0.5f * (t + done) / std::max(seg.length, 1.0e-6f));
      if (trailer)
        hitch = hitchStep(
            *trailer, hitch, static_cast<float>(seg.gear) * (t - done), mid);
      s += t - done;
      done = t;
      push(advance(at, seg, t), k, seg.gear);
    }
    at = advance(at, seg, seg.length);
    path.points.back().pose = at;
  }
}

std::vector<Segment> drivenSteps(std::span<const Pose2> poses,
                                 std::span<const float> along,
                                 std::span<const int> gears)
{
  std::vector<Segment> out;
  const std::size_t n = std::min({poses.size(), along.size(), gears.size()});
  if (n < 2)
    return out;
  out.reserve(n - 1);
  for (std::size_t k = 1; k < n; ++k)
  {
    const float ds = along[k] - along[k - 1];
    if (ds < 1.0e-5f)
      continue; // (a stop to change direction: the same point again)
    const int gear = gears[k] < 0 ? -1 : 1;
    const float k0 = wrapAngle(poses[k].heading - poses[k - 1].heading) /
                     (static_cast<float>(gear) * ds);
    out.push_back({gear, ds, k0, k0});
  }
  return out;
}

std::vector<Segment> drivenSteps(const VehiclePath& path)
{
  std::vector<Pose2> poses;
  std::vector<float> along;
  std::vector<int> gears;
  poses.reserve(path.points.size());
  along.reserve(path.points.size());
  gears.reserve(path.points.size());
  for (const PathPoint& p : path.points)
  {
    poses.push_back(p.pose);
    along.push_back(p.s);
    gears.push_back(p.gear);
  }
  return drivenSteps(poses, along, gears);
}

} // namespace sfs::nav::vehicle
