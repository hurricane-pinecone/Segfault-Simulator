#include "engine/core/ecs/sceneData.h"

#include "../util/typeIds.h"

#include <string_view>

namespace sfs::detail
{

int sceneDataTypeId(std::string_view typeKey)
{
  static TypeIds ids;
  return ids.idOf(typeKey);
}

} // namespace sfs::detail
