#pragma once

#include "engine/runtime/navigation/vehicle/vehicleSearch.h"

#include <vector>

namespace sfs::nav::vehicle
{

/**
 * The smoother (any search's way): from each pose, the farthest pose further
 * on that one continuous-curvature word reaches clear and no slower than the
 * way there, so no zig-zag of the search's motions is left. `goal`: a
 * trailer's angle at the end, if it matters.
 */
std::vector<Segment>
smooth(const Motions& motions, const Way& way, const Goal* goal);

} // namespace sfs::nav::vehicle
