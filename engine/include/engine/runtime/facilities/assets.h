#pragma once

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/ecs/facilityAccess.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/ecs/service.h"
#include "engine/core/ecs/system.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/assetStore/modelData.h"
#include "engine/runtime/game/gameObject.h"
#include "engine/runtime/sceneManager/scene.h"

namespace sfs
{

namespace facility
{

/**
 * The scene's asset loading, as game code uses it (`assets()` on a System,
 * Service, GameObject or Scene): models asked for as the game runs, loaded in
 * the background when the scene loads that way. A value handle over the
 * scene's AssetStore, valid for the step that got it: get it where it's used
 * and keep it in no member. A registry with no scene (a tool or test) has
 * none: its requests fail, with one warning a run (available() tells).
 */
class Assets
{
public:
  /** @return whether the scene loads assets behind this facade */
  inline bool available() const { return m_store != nullptr; }

  /**
   * The asset loading of a registry's scene, as the accessors give it.
   * @param registry the registry (null: none)
   * @return its facade, absent for a registry with no scene
   */
  static Assets in(Registry* registry);

  /**
   * A model, if it's loaded; otherwise starts loading it (in the background
   * when the scene loads that way): ask again later.
   * @param id the model's asset id
   * @param model set to the model once it's Loaded
   * @return whether it's loaded, loading or failed (failed with no store)
   */
  AssetStore::LoadState requestModel(const AssetId& id,
                                     const ModelData** model) const;

private:
  inline explicit Assets(AssetStore* store) : m_store(store) {}

  // The store; null (and warned about once) for a registry with no scene.
  AssetStore* backend() const;

  AssetStore* m_store = nullptr;
};

} // namespace facility

inline facility::Assets System::assets() const
{
  return facility::Assets::in(registry);
}

inline facility::Assets Service::assets() const
{
  return facility::Assets::in(m_registry);
}

inline facility::Assets GameObject::assets() const
{
  return m_scene ? m_scene->assets() : facility::Assets::in(nullptr);
}

inline facility::Assets Scene::assets()
{
  return facility::Assets::in(&m_registry);
}

} // namespace sfs
