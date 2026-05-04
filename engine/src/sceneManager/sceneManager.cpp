#include "engine/assetStore/assetStore.h"
#include <engine/sceneManager/scene.h>
#include <engine/sceneManager/sceneManager.h>
#include <memory>
#include <string>

namespace sfs
{

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

void SceneManager::setAssetStore(AssetStore* assetStore)
{
  m_assetStore = assetStore;
}

} // namespace sfs
