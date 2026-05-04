#pragma once

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

  Scene* createScene();
  Scene* createScene(const std::string& name);
  void destroyScene(SceneId id);
  void destroyScene(const std::string& name);
  void load(SceneId id);
  void load(const std::string& name);

  Scene* current();

  operator bool() { return m_currentScene != nullptr; }

private:
  Scene* m_currentScene = nullptr;
  std::unordered_map<SceneId, std::unique_ptr<Scene>> m_scenes;
  std::unordered_map<std::string, SceneId> m_nameToId;

  SceneId nextSceneId = 0;
};

}; // namespace sfs
