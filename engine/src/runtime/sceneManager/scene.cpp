
#include "engine/core/ecs/entity.h"
#include "engine/core/logger/logger.h"
#include "engine/runtime/input/input.h"
#include <cstdlib>
#include <engine/core/ecs/registry.h>
#include <engine/core/util/profiling.h>
#include <engine/runtime/sceneManager/scene.h>
#include <string>

namespace sfs
{

namespace
{
// A scene asked for a renderer its backend doesn't have: a setup bug, not a
// runtime condition, so name it and stop (as getSystem does).
template <typename T>
T& requireService(T* service, const char* name)
{
  if (service)
    return *service;

  LOG_ERROR(std::string("Scene: the render backend provides no ") + name);
  std::abort();
}
} // namespace

IQuadRenderer& Scene::quadRenderer() const
{
  return requireService(m_quadRenderer, "quad renderer");
}

TextRenderer& Scene::textRenderer() const
{
  return requireService(m_textRenderer, "text renderer");
}

IRenderer3D& Scene::renderer3D() const
{
  return requireService(m_renderer3D, "3D renderer");
}

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

GameObject& Scene::adoptObject(std::unique_ptr<GameObject> object, const Entity& entity)
{
  GameObject& ref = *object;
  ref.m_entity = entity;
  m_gameObjects.push_back(std::move(object));
  ref.onCreate(*this);
  return ref;
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
