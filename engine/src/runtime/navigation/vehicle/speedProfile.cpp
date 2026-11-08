#include "engine/runtime/navigation/vehicle/speedProfile.h"

#include <algorithm>
#include <cmath>

namespace sfs::nav::vehicle
{

void profile(VehiclePath& path,
             const VehicleModel& vehicle,
             float speedCap,
             float startSpeed,
             const std::vector<float>* limits)
{
  std::vector<PathPoint>& p = path.points;
  const std::size_t n = p.size();
  if (n == 0)
    return;
  std::vector<float> v(n);
  for (std::size_t i = 0; i < n; ++i)
  {
    // (How fast curvature changes along here: from the neighbours in gear.)
    const std::size_t a = i > 0 && p[i - 1].gear == p[i].gear ? i - 1 : i;
    const std::size_t b = i + 1 < n && p[i + 1].gear == p[i].gear ? i + 1 : i;
    const float dk = b > a && p[b].s > p[a].s
                         ? (p[b].curvature - p[a].curvature) / (p[b].s - p[a].s)
                         : 0.0f;
    float lim =
        std::min(vehicle.speedAt(p[i].gear, p[i].curvature, dk), speedCap);
    if (limits && i < limits->size())
      lim = std::min(lim, (*limits)[i]);
    v[i] = lim;
  }
  v[0] = std::min(v[0], startSpeed);
  v[n - 1] = 0.0f;
  for (std::size_t i = 1; i < n; ++i)
    if (p[i].gear != p[i - 1].gear)
      v[i - 1] = v[i] = 0.0f; // (a cusp: standing)
  for (std::size_t i = 1; i < n; ++i)
  {
    const float ds = p[i].s - p[i - 1].s;
    v[i] = std::min(
        v[i], std::sqrt(v[i - 1] * v[i - 1] + 2.0f * vehicle.accel * ds));
  }
  for (std::size_t i = n - 1; i-- > 0;)
  {
    const float ds = p[i + 1].s - p[i].s;
    v[i] = std::min(
        v[i], std::sqrt(v[i + 1] * v[i + 1] + 2.0f * vehicle.brake * ds));
  }
  float t = 0.0f;
  p[0].speed = v[0];
  p[0].time = 0.0f;
  for (std::size_t i = 1; i < n; ++i)
  {
    const float ds = p[i].s - p[i - 1].s;
    const float mean = 0.5f * (v[i] + v[i - 1]);
    t += ds > 0.0f ? ds / std::max(mean, 0.05f) : 0.0f;
    if (p[i].gear != p[i - 1].gear)
      t += vehicle.shuttleSeconds; // (the braking and pulling away: above)
    p[i].speed = v[i];
    p[i].time = t;
  }
}

} // namespace sfs::nav::vehicle
