
#include "engine/core/components/inputContextComponent.h"
#include "engine/core/components/nameComponent.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/logger/logger.h"
#include "engine/core/util/cpuTiming.h"
#include "engine/core/util/fatalError.h"
#include "engine/runtime/input/input.h"
#include <algorithm>
#include <climits>
#include <cmath>
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
// An active input context's keys, as read before input is handed out.
struct KeyClaim
{
  Entity owner;
  int priority = 0;
  std::vector<Key> keys;
};

// The active contexts' claims, logging an unknown key name once a context.
std::vector<KeyClaim> activeClaims(Registry& registry)
{
  std::vector<KeyClaim> claims;
  for (const Entity& entity : registry.query<InputContextComponent>())
  {
    auto& context = entity.getComponent<InputContextComponent>();
    if (!context.active || context.keys.empty())
      continue;
    KeyClaim claim{entity, context.priority, {}};
    claim.keys.reserve(context.keys.size());
    for (const std::string& name : context.keys)
      if (const std::optional<Key> key = keyFromName(name))
        claim.keys.push_back(*key);
      else if (!context.loggedUnknown)
      {
        context.loggedUnknown = true;
        LOG_WARN("InputContextComponent: no key is named '" + name + "'");
      }
    claims.push_back(std::move(claim));
  }
  return claims;
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
  m_registry.destroyEntity(entity);
  for (const Entity& doomed : hierarchy::descendants(entity))
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
  const std::size_t slash = nameOrPath.find('/');
  const std::string_view first = nameOrPath.substr(0, slash);
  Entity found;
  if (slash == std::string_view::npos)
  {
    for (const HierarchyLinks::Placed& placed : hierarchy::preorder(m_registry))
      if (const Entity entity = m_registry.getEntity(placed.id);
          named(entity, first))
        return entity;
    return {};
  }

  // A path: its first part is matched among the roots, the rest down from it.
  for (const Entity& root : hierarchy::roots(m_registry))
    if (named(root, first))
    {
      found = root;
      break;
    }

  std::string_view rest = nameOrPath;
  for (std::size_t next = slash;
       found.isValid() && next != std::string_view::npos;)
  {
    rest = rest.substr(next + 1);
    next = rest.find('/');
    const std::string_view part = rest.substr(0, next);
    const Entity parent = found;
    found = {};
    for (const Entity& child : hierarchy::childrenOf(parent))
      if (named(child, part))
      {
        found = child;
        break;
      }
  }
  return found;
}

std::vector<Entity> Scene::findEntities(std::string_view name)
{
  std::vector<Entity> found;
  for (const HierarchyLinks::Placed& placed : hierarchy::preorder(m_registry))
    if (const Entity entity = m_registry.getEntity(placed.id);
        named(entity, name))
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
  // The frame's game time, in as many steps as the Stepped systems need
  // (the others run once, on the last).
  double gameTime = deltaTime * m_registry.timeScale();
  {
    ZoneScopedN("Scene: systems update");
    int steps = 1;
    const double maxStep = m_registry.maxTimeStep();
    if (maxStep > 0.0 && gameTime > maxStep)
    {
      steps = std::min(static_cast<int>(std::ceil(gameTime / maxStep)),
                       m_registry.mostTimeSteps());
      gameTime = std::min(gameTime, maxStep * steps);
    }
    const double step = gameTime / steps;
    for (int i = 0; i < steps; ++i)
    {
      const bool last = i + 1 == steps;
      m_registry.advanceGameTime(step);
      m_registry.forEachSystem(
          [&](System& system)
          {
            if (!system.enabled())
              return;
            const System::TimeStep kind = system.timeStep();
            if (kind != System::TimeStep::Stepped && !last)
              return;
            const cpuTiming::Scope timed(
                cpuTiming::enabled()
                    ? cpuTiming::label("update/", typeid(system))
                    : nullptr);
            system.update(kind == System::TimeStep::Stepped ? step
                          : kind == System::TimeStep::Frame ? gameTime
                                                            : deltaTime);
          });
    }
  }

  // By index, and only those there now: an object may create others (which
  // grows the list), and the ones it makes start next frame.
  for (std::size_t i = 0, count = m_gameObjects.size(); i < count; ++i)
  {
    GameObject& object = *m_gameObjects[i];
    if (object.m_destroyed)
      continue;
    const cpuTiming::Scope timed(
        cpuTiming::enabled() ? cpuTiming::label("update/", typeid(object))
                             : nullptr);
    object.onUpdate(deltaTime);
  }

  {
    SFS_CPU_SCOPE("update/scene");
    onUpdate(deltaTime);
  }

  ZoneScopedN("Scene: systems late update");
  m_registry.forEachSystem(
      [&](System& system)
      {
        if (!system.enabled())
          return;
        const cpuTiming::Scope timed(
            cpuTiming::enabled()
                ? cpuTiming::label("lateUpdate/", typeid(system))
                : nullptr);
        system.lateUpdate(
            system.timeStep() == System::TimeStep::Real ? deltaTime : gameTime);
      });
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
  // The keys input contexts claim, read once: each object gets the keyboard
  // without the keys claimed by contexts that outrank its own.
  const std::vector<KeyClaim> claims = activeClaims(m_registry);
  Input own;
  {
    const ObjectCalls calls(*this);
    for (std::size_t i = 0, count = m_gameObjects.size(); i < count; ++i)
    {
      GameObject& object = *m_gameObjects[i];
      if (object.m_destroyed)
        continue;
      if (claims.empty())
      {
        object.onProcessInput(seen);
        continue;
      }
      const Entity& entity = object.entity();
      const auto* context =
          entity.isAlive() && entity.hasComponent<InputContextComponent>()
              ? &entity.getComponent<InputContextComponent>()
              : nullptr;
      const int rank = context && context->active ? context->priority : INT_MIN;
      own = seen;
      for (const KeyClaim& claim : claims)
        if (claim.owner != entity && claim.priority > rank)
          for (const Key key : claim.keys)
            own.keyboard().withhold(key);
      object.onProcessInput(own);
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
        if (!system.enabled())
          return;
        const cpuTiming::Scope timed(
            cpuTiming::enabled() ? cpuTiming::label("render/", typeid(system))
                                 : nullptr);
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
