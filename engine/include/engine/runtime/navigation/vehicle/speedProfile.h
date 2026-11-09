#pragma once

#include "engine/runtime/navigation/vehicle/vehicleModel.h"
#include "engine/runtime/navigation/vehicle/vehiclePath.h"

#include <limits>
#include <vector>

namespace sfs::nav::vehicle
{

/**
 * The speed profile of a sampled path: at every point at most
 * - the gear's top speed (and `speedCap`, and `limits[i]` when given: a
 *   caller's limit per point, e.g. a road's or a working pace),
 * - sqrt(lateralAccel / |curvature|) (slow for tight curves),
 * - curvatureRate / |dcurvature/ds| (slow where the wheel must turn fast),
 * standing at every cusp and at the end, reached and left within accel and
 * brake. Fills PathPoint::speed and ::time (a cusp's standing,
 * VehicleModel::shuttleSeconds, included).
 */
void profile(VehiclePath& path,
             const VehicleModel& vehicle,
             float speedCap = std::numeric_limits<float>::infinity(),
             float startSpeed = 0.0f,
             const std::vector<float>* limits = nullptr);

/** How long the path takes by its profile. */
inline float seconds(const VehiclePath& path) { return path.duration(); }

} // namespace sfs::nav::vehicle
