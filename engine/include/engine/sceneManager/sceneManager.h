#pragma once

#include "engine/assetStore/assetStore.h"
#include "engine/sceneManager/scene.h"
#include <SDL_render.h>
#include <memory>
#include <unordered_map>

namespace sfs
{

class SceneManager
{
public:
  SceneManager() = default;
  ~SceneManager() = default;

  template <typename TScene = Scene, typename... TArgs>
  TScene* createScene(TArgs&&... args);

  void destroyScene(SceneId id);
  void destroyScene(const std::string& name);
  void load(SceneId id);
  void load(const std::string& name);

  Scene* current();

  void setAssetStore(AssetStore* assetStore);

  operator bool() { return m_currentScene != nullptr; }

private:
  Scene* m_currentScene = nullptr;
  std::unordered_map<SceneId, std::unique_ptr<Scene>> m_scenes;
  std::unordered_map<std::string, SceneId> m_nameToId;

  AssetStore* m_assetStore = nullptr;

  SceneId nextSceneId = 0;
};

template <typename TScene, typename... TArgs>
TScene* SceneManager::createScene(TArgs&&... args)
{
  if (!m_assetStore)
    return nullptr;

  SceneId id = nextSceneId++;

  auto scene =
      std::make_unique<TScene>(id, *m_assetStore, std::forward<TArgs>(args)...);

  TScene* ptr = scene.get();

  if (m_scenes.empty())
  {
    m_currentScene = ptr;
    m_currentScene->onEnter();
  }

  m_scenes.emplace(id, std::move(scene));
  m_nameToId[ptr->name()] = id;

  static_cast<Scene*>(ptr)->onInit();

  return ptr;
}

}; // namespace sfs
