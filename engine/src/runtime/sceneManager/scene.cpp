
#include "engine/core/components/nameComponent.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/logger/logger.h"
#include "engine/core/util/fatalError.h"
#include "engine/runtime/input/input.h"
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

  fatalError(std::string("Scene: the render backend provides no ") + name);
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
  fatalError("Scene: game() called on a scene that isn't running in a game");
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

  flushEntities();
  {
    const ObjectCalls calls(*this);
    updateObjects(deltaTime);
  }
  deleteDestroyedObjects();
}

void Scene::updateObjects(double deltaTime)
{
  {
    ZoneScopedN("Scene: systems update");
    m_registry.forEachSystem(
        [deltaTime](System& system)
        {
          if (system.enabled())
            system.update(deltaTime);
        });
  }

  // By index, and only those there now: an object may create others (which
  // grows the list), and the ones it makes start next frame.
  for (std::size_t i = 0, count = m_gameObjects.size(); i < count; ++i)
  {
    GameObject& object = *m_gameObjects[i];
    if (!object.m_destroyed)
      object.onUpdate(deltaTime);
  }

  onUpdate(deltaTime);
}

void Scene::flushEntities()
{
  m_registry.flushEntities();
  destroyObjectsOfDeadEntities();
  deleteDestroyedObjects();
}

GameObject& Scene::adoptObject(std::unique_ptr<GameObject> object,
                               const Entity& entity)
{
  GameObject& ref = *object;
  ref.m_entity = entity;
  ref.m_scene = this;
  m_gameObjects.push_back(std::move(object));
  const ObjectCalls calls(*this); // not deleted under its own onCreate
  ref.onCreate(*this);
  return ref;
}

void Scene::processInput(const Input& input)
{
  ZoneScopedN("Scene::processInput");

  // What the scene takes first (its UI's clicks) the objects don't see.
  Input seen = input;
  filterInput(seen);
  {
    const ObjectCalls calls(*this);
    for (std::size_t i = 0, count = m_gameObjects.size(); i < count; ++i)
    {
      GameObject& object = *m_gameObjects[i];
      if (!object.m_destroyed)
        object.onProcessInput(seen);
    }
    onProcessInput(seen);
  }
  deleteDestroyedObjects();
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
      if (object.m_destroyed || object.m_entity != self)
        continue;
      if (contact.trigger)
        begun ? object.onTriggerEnter(collision)
              : object.onTriggerExit(collision);
      else
        begun ? object.onCollisionEnter(collision)
              : object.onCollisionExit(collision);
    }
  };
  {
    const ObjectCalls calls(*this);
    tell(contact.a, contact.b);
    tell(contact.b, contact.a);
  }
  deleteDestroyedObjects();
}

void Scene::notifyContact(const Contact2D& contact, bool begun)
{
  const auto tell =
      [&](const Entity& self, const Entity& other, glm::vec2 normal)
  {
    if (!self.isValid())
      return;
    const Collision2D collision{other, normal};
    // By index, as above.
    for (std::size_t i = 0, count = m_gameObjects.size();
         i < count && i < m_gameObjects.size();
         ++i)
    {
      GameObject& object = *m_gameObjects[i];
      if (object.m_entity != self)
        continue;
      if (contact.trigger)
        begun ? object.onTriggerEnter2D(collision)
              : object.onTriggerExit2D(collision);
      else
        begun ? object.onCollisionEnter2D(collision)
              : object.onCollisionExit2D(collision);
    }
  };
  tell(contact.a, contact.b, contact.normal);
  tell(contact.b, contact.a, -contact.normal);
}

void Scene::destroyObject(GameObject* object)
{
  if (!object || object->m_destroyed || object->m_scene != this)
    return;

  object->m_destroyed = true;
  m_objectsDestroyed = true;
  object->destroy(*this);
  deleteDestroyedObjects();
}

void Scene::destroyObjects()
{
  for (const std::unique_ptr<GameObject>& object : m_gameObjects)
    object->m_destroyed = true;
  m_objectsDestroyed = !m_gameObjects.empty();
  deleteDestroyedObjects();
}

void Scene::destroyObjectsOfDeadEntities()
{
  for (const std::unique_ptr<GameObject>& object : m_gameObjects)
    if (!object->m_destroyed && object->m_entity.isValid() &&
        !object->m_entity.isAlive())
    {
      object->m_destroyed = true;
      m_objectsDestroyed = true;
    }
}

void Scene::deleteDestroyedObjects()
{
  // An onDestroy may destroy more: they're deleted in the next round.
  while (m_objectsDestroyed && m_objectCalls == 0)
  {
    m_objectsDestroyed = false;
    std::vector<std::unique_ptr<GameObject>> doomed;
    for (std::unique_ptr<GameObject>& object : m_gameObjects)
      if (object->m_destroyed)
        doomed.push_back(std::move(object));
    std::erase(m_gameObjects, nullptr);

    const ObjectCalls calls(*this);
    for (const std::unique_ptr<GameObject>& object : doomed)
      object->onDestroy();
  }
}

} // namespace sfs
