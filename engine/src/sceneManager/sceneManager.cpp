#include <engine/sceneManager/scene.h>
#include <engine/sceneManager/sceneManager.h>
#include <memory>
#include <string>

namespace sfs
{

Scene* SceneManager::createScene()
{
  std::string name = "Scene_" + std::to_string(nextSceneId);
  return createScene(name);
}

Scene* SceneManager::createScene(const std::string& name)
{
  SceneId id = nextSceneId++;
  auto scene = std::make_unique<Scene>(id, name);
  Scene* ptr = scene.get();

  if (m_scenes.empty())
    m_currentScene = scene.get();

  m_scenes.emplace(id, std::move(scene));
  m_nameToId[name] = id;

  return ptr;
}

void SceneManager::destroyScene(SceneId id)
{
  auto it = m_scenes.find(id);

  if (it == m_scenes.end())
    return;

  if (m_currentScene && m_currentScene->id() == id)
    return;

  m_nameToId.erase(it->second->name());
  m_scenes.erase(it);
}

void SceneManager::destroyScene(const std::string& name)
{
  auto it = m_nameToId.find(name);

  if (it == m_nameToId.end())
    return;

  destroyScene(it->second);
  m_nameToId.erase(name);
}

void SceneManager::load(SceneId id)
{
  auto it = m_scenes.find(id);

  if (it == m_scenes.end())
    return;

  if (m_currentScene)
    m_currentScene->onExit();

  m_currentScene = it->second.get();
  m_currentScene->onEnter();
}

void SceneManager::load(const std::string& name)
{
  auto it = m_nameToId.find(name);

  if (it == m_nameToId.end())
    return;

  load(it->second);
}

Scene* SceneManager::current() { return m_currentScene; }

} // namespace sfs
