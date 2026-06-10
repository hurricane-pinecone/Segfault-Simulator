#pragma once

#include "engine/core/logger/logger.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/sceneManager/scene.h"
#include <SDL_render.h>
#include <memory>
#include <unordered_map>

namespace sfs
{

class IQuadRenderer;
class TextRenderer;

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

  // The drawing/asset services scenes are constructed against, supplied by the
  // active render backend. Must be set (Game::setup does this) before any scene
  // is created.
  void setSceneServices(SceneServices* services);

  operator bool() { return m_currentScene != nullptr; }

private:
  Scene* m_currentScene = nullptr;
  std::unordered_map<SceneId, std::unique_ptr<Scene>> m_scenes;
  std::unordered_map<std::string, SceneId> m_nameToId;

  SceneServices* m_services = nullptr;

  SceneId nextSceneId = 0;
};

template <typename TScene, typename... TArgs>
TScene* SceneManager::createScene(TArgs&&... args)
{
  // Services must be wired (Game::setup does this) before a scene exists: they
  // are constructor-injected as references, so a missing bundle can't be
  // deferred to a null check later.
  if (!m_services)
  {
    LOG_ERROR("createScene called before engine services were set");
    return nullptr;
  }

  SceneId id = nextSceneId++;

  auto scene =
      std::make_unique<TScene>(id, *m_services, std::forward<TArgs>(args)...);

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
