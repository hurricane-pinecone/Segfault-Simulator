#pragma once

#include "engine/assetStore/assetStore.h"
#include "engine/ecs/entity.h"
#include "engine/ecs/registry.h"
#include "engine/game/gameObject.h"
#include "engine/input/input.h"
#include <SDL_render.h>
#include <memory>
#include <string>
#include <vector>

namespace sfs
{

using SceneId = uint32_t;

class Scene
{
public:
  Scene(SceneId id, AssetStore& assetStore)
      : m_assetStore(assetStore), m_id(id),
        m_name("scene_" + std::to_string(id))
  {
  }

  Scene(SceneId id, AssetStore& assetStore, const std::string& name)
      : m_assetStore(assetStore), m_id(id), m_name(name)
  {
  }

  virtual ~Scene() = default;

  enum class Mode
  {
    EDITOR,
    GAME,
  };

  SceneId id() const;
  const std::string& name() const;

  void update(double deltaTime);
  void processInput(const sfs::Input& input);
  void render();

  Entity createEntity();
  Entity getEntity(int id);
  void destroyEntity(int id);

  AssetStore& assetStore() const { return m_assetStore; }

  template <typename TObject, typename... TArgs>
  TObject& createObject(TArgs&&... args);

  template <typename TSystem, typename... TArgs>
  TSystem& addSystem(TArgs&&... args);

  template <typename TSystem>
  void removeSystem();

  template <typename TSystem>
  bool hasSystem() const;

  template <typename TSystem>
  TSystem& getSystem() const;

private:
  Registry& registry();
  const Registry& registry() const;

protected:
  virtual void onInit() {};
  virtual void onEnter() {};
  virtual void onExit() {};
  virtual void onUpdate(double deltaTime) {};
  virtual void onRender() {};
  virtual void onPostRender() {};
  virtual void onProcessInput(const Input& input) {};

  friend class SceneManager;

protected:
  AssetStore& m_assetStore;

private:
  SceneId m_id = 0;
  std::string m_name = "";
  Registry m_registry;

  std::vector<std::unique_ptr<GameObject>> m_gameObjects;
};

template <typename TObject = GameObject, typename... TArgs>
TObject& Scene::createObject(TArgs&&... args)
{
  auto object = std::make_unique<TObject>(std::forward<TArgs>(args)...);
  TObject& ref = *object;

  m_gameObjects.push_back(std::move(object));
  ref.onCreate(*this);

  return ref;
}

template <typename TSystem, typename... TArgs>
TSystem& Scene::addSystem(TArgs&&... args)
{
  return m_registry.addSystem<TSystem>(std::forward<TArgs>(args)...);
}

template <typename TSystem>
void Scene::removeSystem()
{
  m_registry.removeSystem<TSystem>();
}

template <typename TSystem>
bool Scene::hasSystem() const
{
  return m_registry.hasSystem<TSystem>();
}

template <typename TSystem>
TSystem& Scene::getSystem() const
{
  return m_registry.getSystem<TSystem>();
}

}; // namespace sfs
