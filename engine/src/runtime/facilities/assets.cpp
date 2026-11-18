#include "engine/runtime/facilities/assets.h"

#include "absentBackend.h"
#include "engine/core/assets/assetDatabase.h"
#include "engine/core/ecs/facilityAccess.h"
#include "engine/core/ecs/registry.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/assetStore/modelData.h"

#include <atomic>

namespace sfs::facility
{

namespace
{
std::atomic<bool> s_warned{false};
} // namespace

AssetStore* Assets::backend() const
{
  if (!m_store)
    warnAbsent(s_warned, "assets", "AssetStore (a registry with no scene)");

  return m_store;
}

Assets Assets::in(Registry* registry)
{
  return Assets(registry ? detail::HostAccess::assetStore(*registry) : nullptr);
}

AssetStore::LoadState Assets::requestModel(const AssetId& id,
                                           const ModelData** model) const
{
  AssetStore* store = backend();
  return store ? store->requestModel(id, model) : AssetStore::LoadState::Failed;
}

} // namespace sfs::facility
