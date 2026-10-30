
#include "engine/core/components/nameComponent.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/hierarchy/hierarchy.h"
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

IRenderer& Scene::renderer() const
{
  return requireService(m_renderer, "renderer");
}

Game& Scene::game() const
{
  if (m_game)
    return *m_game;
  LOG_ERROR("Scene: game() called on a scene that isn't running in a game");
  std::abort();
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

void Scene::destroyBranch(const Entity& entity)
{
  for (const Entity& doomed : hierarchy::EntityTree(entities()).subtree(entity))
    m_registry.destroyEntity(doomed);
}

namespace
{
bool named(const Entity& entity, std::string_view name)
{
  return entity.hasComponent<NameComponent>() &&
         entity.getComponent<NameComponent>().name == name;
}
} // namespace

Entity Scene::findEntity(std::string_view nameOrPath)
{
  const hierarchy::EntityTree tree(entities());
  if (nameOrPath.find('/') == std::string_view::npos)
  {
    for (const Entity& entity : tree.preOrder())
      if (named(entity, nameOrPath))
        return entity;
    return {};
  }

  // A path: match each part among the previous match's children.
  const std::vector<Entity>* candidates = &tree.roots();
  Entity found;
  std::string_view rest = nameOrPath;
  while (true)
  {
    const std::size_t slash = rest.find('/');
    const std::string_view part = rest.substr(0, slash);
    found = {};
    for (const Entity& entity : *candidates)
      if (named(entity, part))
      {
        found = entity;
        break;
      }
    if (!found.isValid() || slash == std::string_view::npos)
      return found;
    candidates = &tree.children(found);
    rest = rest.substr(slash + 1);
  }
}

std::vector<Entity> Scene::findEntities(std::string_view name)
{
  std::vector<Entity> found;
  for (const Entity& entity : hierarchy::EntityTree(entities()).preOrder())
    if (named(entity, name))
      found.push_back(entity);
  return found;
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

GameObject& Scene::adoptObject(std::unique_ptr<GameObject> object,
                               const Entity& entity)
{
  GameObject& ref = *object;
  ref.m_entity = entity;
  ref.m_scene = this;
  m_gameObjects.push_back(std::move(object));
  ref.onCreate(*this);
  return ref;
}

void Scene::processInput(const Input& input)
{
  ZoneScopedN("Scene::processInput");

  // What the scene takes first (its UI's clicks) the objects don't see.
  Input seen = input;
  filterInput(seen);
  for (auto& obj : m_gameObjects)
  {
    obj->onProcessInput(seen);
  }
  onProcessInput(seen);
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

void Scene::notifyContact(const Contact3D& contact, bool begun)
{
  const auto tell = [&](const Entity& self, const Entity& other)
  {
    if (!self.isValid())
      return;
    const Collision3D collision{other};
    // By index: a handler may create objects (which would invalidate an
    // iterator); ones it adds aren't told about this contact.
    for (std::size_t i = 0, count = m_gameObjects.size();
         i < count && i < m_gameObjects.size();
         ++i)
    {
      GameObject& object = *m_gameObjects[i];
      if (object.m_entity != self)
        continue;
      if (contact.trigger)
        begun ? object.onTriggerEnter(collision)
              : object.onTriggerExit(collision);
      else
        begun ? object.onCollisionEnter(collision)
              : object.onCollisionExit(collision);
    }
  };
  tell(contact.a, contact.b);
  tell(contact.b, contact.a);
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
