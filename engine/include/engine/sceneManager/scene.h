#pragma once

#include "engine/ecs/entity.h"
#include "engine/ecs/registry.h"
#include <SDL_render.h>

namespace sfs
{

using SceneId = uint32_t;

class Scene
{
public:
  Scene(SceneId id) : m_id(id) {};
  Scene(SceneId id, const std::string& name) : m_id(id), m_name(name) {};
  ~Scene() = default;

  enum class Mode
  {
    EDITOR,
    GAME,
  };

  SceneId id();
  const std::string& name();

  // TODO: Are these even needed
  void onEnter() {};
  void onExit() {};

  void update(double deltaTime);
  void render(SDL_Renderer& renderer);

  Registry& registry();
  const Registry& registry() const;

  Entity createEntity();
  Entity getEntity(int id);
  void destroyEntity(int id);

  template <typename TSystem, typename... TArgs>
  void addSystem(TArgs&&... args);

  template <typename TSystem>
  void removeSystem();

  template <typename TSystem>
  bool hasSystem() const;

  template <typename TSystem>
  TSystem& getSystem() const;

private:
  SceneId m_id = -1;
  Registry m_registry;
  std::string m_name = "";
};

template <typename TSystem, typename... TArgs>
void Scene::addSystem(TArgs&&... args)
{
  m_registry.addSystem<TSystem>(args...);
}

template <typename TSystem>
void Scene::removeSystem()
{
  m_registry.removeSystem<TSystem>();
}

template <typename TSystem>
bool Scene::hasSystem() const
{
  m_registry.hasSystem<TSystem>();
}

template <typename TSystem>
TSystem& Scene::getSystem() const
{
  return m_registry.getSystem<TSystem>();
}

}; // namespace sfs
