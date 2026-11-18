#pragma once

#include <atomic>
#include <string_view>

namespace sfs::facility
{

/**
 * Logs, once per process, that a facility was used without the engine part
 * behind it (a scene without it, or no scene at all): its commands do nothing
 * and its queries give empty answers.
 * @param warned the facility's own flag, set by the first warning
 * @param accessor the accessor's name ("physics")
 * @param backend the missing part's name ("PhysicsSystem3D")
 */
void warnAbsent(std::atomic<bool>& warned,
                std::string_view accessor,
                std::string_view backend);

} // namespace sfs::facility
