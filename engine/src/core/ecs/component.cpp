#include "engine/core/ecs/component.h"

#include "engine/core/logger/logger.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace sfs::detail
{

int componentTypeId(std::string_view typeKey)
{
  static std::mutex mutex;
  static std::unordered_map<std::string, int> ids;

  const std::lock_guard lock(mutex);

  const auto [it, inserted] =
      ids.try_emplace(std::string(typeKey), static_cast<int>(ids.size()));

  // The id is a bit index into a fixed-width Signature; an id at or past the
  // ceiling has no bit to represent it. Report it here, where the type is
  // known, rather than as an opaque out-of-range thrown deep in the registry.
  if (inserted && it->second >= static_cast<int>(MAX_COMPONENTS))
    LOG_ERROR("Component type id " + std::to_string(it->second) + " for " +
              std::string(typeKey) + " exceeds MAX_COMPONENTS (" +
              std::to_string(MAX_COMPONENTS) + "); raise MAX_COMPONENTS in entity.h");

  return it->second;
}

} // namespace sfs::detail
