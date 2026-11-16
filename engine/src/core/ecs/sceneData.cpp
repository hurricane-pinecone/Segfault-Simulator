#include "engine/core/ecs/sceneData.h"

#include "../util/typeIds.h"

namespace sfs::detail
{

int sceneDataTypeId(std::string_view typeKey)
{
  static TypeIds ids;
  return ids.idOf(typeKey);
}

} // namespace sfs::detail
