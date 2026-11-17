#include "engine/core/ecs/component.h"

#include "../util/typeIds.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/logger/logger.h"
#include "engine/core/util/debugFeatures.h"
#include "engine/core/util/fatalError.h"
#include "engine/core/util/typeKey.h"

#include <string>
#include <string_view>

namespace sfs::detail
{

int componentTypeId(std::string_view typeKey)
{
  static TypeIds ids;
  bool added = false;
  const int id = ids.idOf(typeKey, &added);

  // Development builds warn while there's still room to raise the limit.
  constexpr int HEADROOM = 8;
  const int limit = static_cast<int>(MAX_COMPONENTS);

  if (added && debugFeatures::compiled() && id >= limit - HEADROOM &&
      id < limit)
    LOG_WARN("Component type " + readableTypeName(typeKey) + " is number " +
             std::to_string(id + 1) + " of MAX_COMPONENTS (" +
             std::to_string(limit) + "); raise MAX_COMPONENTS in entity.h");

  // The id is a bit index into a fixed-width Signature; an id at or past the
  // ceiling has no bit to represent it. Stop here, where the type is known,
  // rather than with an opaque out-of-range deep in the registry. Every time:
  // the caller's cached id isn't set when this throws, so it asks again.
  if (id >= static_cast<int>(MAX_COMPONENTS))
    fatalError("Too many component types: " + readableTypeName(typeKey) +
               " would be number " + std::to_string(id + 1) +
               ", past MAX_COMPONENTS (" + std::to_string(MAX_COMPONENTS) +
               "); raise MAX_COMPONENTS in entity.h");

  return id;
}

int systemTypeId(std::string_view typeKey)
{
  static TypeIds ids;
  return ids.idOf(typeKey);
}

} // namespace sfs::detail
