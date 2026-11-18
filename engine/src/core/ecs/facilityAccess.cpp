#include "engine/core/ecs/facilityAccess.h"

#include "engine/core/ecs/registry.h"

namespace sfs::detail
{

AssetStore* HostAccess::assetStore(const Registry& registry)
{
  return registry.m_assetStore;
}

} // namespace sfs::detail
