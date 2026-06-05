
#include "engine/core/ecs/entity.h"
#include "engine/runtime/input/input.h"
#include <engine/core/ecs/registry.h>
#include <engine/runtime/sceneManager/scene.h>
#include <engine/core/util/profiling.h>
#include <string>

namespace sfs
{

SceneId Scene::id() const { return m_id; }
const std::string& Scene::name() const { return m_name; }

Registry& Scene::registry() { return m_registry; }
const Registry& Scene::registry() const { return m_registry; }

Entity Scene::createEntity() { return m_registry.createEntity(); }
Entity Scene::getEntity(Entity::EntityId id)
{
  return m_registry.getEntity(id);
}
void Scene::destroyEntity(const Entity& entity)
{
  m_registry.destroyEntity(entity);
}

void Scene::update(double deltaTime)
{
  ZoneScopedN("Scene::update");

  m_registry.flushEntities();

  {
    ZoneScopedN("Scene: systems update");
    m_registry.forEachSystem(
        [deltaTime](System& system)
        {
          if (system.enabled())
            system.update(deltaTime);
        });
  }

  for (auto& obj : m_gameObjects)
  {
    obj->onUpdate(deltaTime);
  }

  onUpdate(deltaTime);
}

void Scene::processInput(const Input& input)
{
  ZoneScopedN("Scene::processInput");

  for (auto& obj : m_gameObjects)
  {
    obj->onProcessInput(input);
  }
  onProcessInput(input);
}
void Scene::render()

{
  ZoneScopedN("Scene::render");

  m_registry.forEachSystem(
      [](System& system)
      {
        if (system.enabled())
          system.render();
      });

  onRender();
}

void Scene::destroyObject(GameObject* object)
{
  if (!object)
    return;

  object->destroy(*this);

  m_gameObjects.erase(
      std::remove_if(m_gameObjects.begin(),
                     m_gameObjects.end(),
                     [object](const std::unique_ptr<GameObject>& current)
                     { return current.get() == object; }),
      m_gameObjects.end());
}

} // namespace sfs
