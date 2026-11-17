
#include "engine/runtime/sceneManager/scene.h"

#include "engine/core/components/contactReportComponent.h"
#include "engine/core/components/inputContextComponent.h"
#include "engine/core/components/nameComponent.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/hierarchyLinks.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/ecs/system.h"
#include "engine/core/events/eventOwner.h"
#include "engine/core/events/events.h"
#include "engine/core/events/eventVerbs.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/logger/logger.h"
#include "engine/core/reflection/reflection.h"
#include "engine/core/util/cpuTiming.h"
#include "engine/core/util/fatalError.h"
#include "engine/core/util/profiling.h"
#include "engine/runtime/game/gameObject.h"
#include "engine/runtime/input/input.h"
#include "engine/runtime/input/keyboardInput.h"
#include "engine/runtime/systems/collision2D.h"
#include "engine/runtime/systems/collision3D.h"

#include <glm/glm/ext/vector_float2.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

namespace sfs
{

namespace
{
// An active input context's keys, as read before input is handed out.
struct KeyClaim
{
  Entity owner;
  int priority = 0;
  std::vector<Key> keys;
};

// A scene asked for a renderer its backend doesn't have: a setup bug, not a
// runtime condition, so name it and stop (as getSystem does).
template <typename T>
T& requireBackend(T* backend, const char* name)
{
  if (backend)
    return *backend;

  fatalError(std::string("Scene: the render backend provides no ") + name);
}

// The active contexts' claims, logging an unknown key name once a context.
std::vector<KeyClaim> activeClaims(Registry& registry)
{
  std::vector<KeyClaim> claims;

  for (auto [entity, context] : registry.query<InputContextComponent>())
  {
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

void* Scene::addSceneData(const reflect::TypeInfo& type)
{
  return type.sceneData.add ? type.sceneData.add(m_registry) : nullptr;
}

Entity Scene::createEntity() { return m_registry.createEntity(); }

void Scene::destroyBranch(const Entity& entity)
{
  m_registry.destroyEntity(entity);

  for (const Entity& doomed : hierarchy::descendants(entity))
    m_registry.destroyEntity(doomed);
}

void Scene::destroyEntity(const Entity& entity)
{
  m_registry.destroyEntity(entity);
}

Game& Scene::game() const
{
  if (m_game)
    return *m_game;

  fatalError("Scene: game() called on a scene that isn't running in a game");
}

Entity Scene::getEntity(Entity::EntityId id)
{
  return m_registry.getEntity(id);
}

const std::vector<Entity>& Scene::holdersOf(const reflect::TypeInfo& type)
{
  static const std::vector<Entity> kNone;
  return type.component.holders ? type.component.holders(m_registry) : kNone;
}

SceneId Scene::id() const { return m_id; }
const std::string& Scene::name() const { return m_name; }

Registry& Scene::registry() { return m_registry; }
const Registry& Scene::registry() const { return m_registry; }

void Scene::removeSceneData(const reflect::TypeInfo& type)
{
  if (type.sceneData.remove)
    type.sceneData.remove(m_registry);
}

IRenderer& Scene::renderer() const
{
  return requireBackend(m_renderer, "renderer");
}

void* Scene::sceneDataOf(const reflect::TypeInfo& type)
{
  return type.sceneData.get ? type.sceneData.get(m_registry) : nullptr;
}

namespace
{
bool named(const Entity& entity, std::string_view name)
{
  const NameComponent* component = entity.getComponent<NameComponent>();
  return component && component->name == name;
}
} // namespace

GameObject& Scene::adoptObject(std::unique_ptr<GameObject> object,
                               const Entity& entity)
{
  GameObject& ref = *object;
  ref.m_entity = entity;
  ref.m_scene = this;
  m_gameObjects.push_back(std::move(object));
  create(ref);
  return ref;
}

void Scene::create(GameObject& object)
{
  const ObjectCalls calls(*this); // not deleted under its own onCreate
  // The handles it makes in onCreate are its own (an onCreate may make
  // other objects, so the one before is put back after).
  Events& events = m_registry.events();
  detail::EventOwner* const before = events.settingUpOwner();
  const std::type_info* const beforeType = events.settingUpType();
  detail::EventHost::settingUp(events, &object.m_eventOwner, &typeid(object));
  object.onCreate(*this);
  detail::EventHost::settingUp(events, before, beforeType);
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
    if (!object->m_destroyed && object->m_entity != Entity{} &&
        !object->m_entity.isValid())
    {
      object->m_destroyed = true;
      m_objectsDestroyed = true;
    }
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

void Scene::flushEntities()
{
  m_registry.flushEntities();
  destroyObjectsOfDeadEntities();
  deleteDestroyedObjects();
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
      const auto* context = entity.getComponent<InputContextComponent>();
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

void Scene::update(double deltaTime)
{
  ZoneScopedN("Scene::update");

  flushEntities();
  // The events' frame starts here: what the flush sent (objects' onDestroy)
  // belongs to the frame before, what's sent from now on to this one.
  m_registry.events().update();
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

      // Responses workers sent during the last step are read at this one, as
      // a plan that finishes mid-frame would be by polling.
      if (i > 0)
        m_registry.events().takeIn();

      m_registry.advanceGameTime(step);
      // Services first, so what they change reaches the systems this step.
      m_registry.runServices();
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
            m_registry.updateSystem(system,
                                    kind == System::TimeStep::Stepped ? step
                                    : kind == System::TimeStep::Frame
                                        ? gameTime
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
    // Its event handlers run first, with the span its onUpdate reads.
    detail::EventOwner& owner = object.m_eventOwner;
    owner.begin();
    owner.dispatch();

    // A handler may have destroyed it.
    if (!object.m_destroyed)
      object.onUpdate(deltaTime);

    owner.end();
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
        m_registry.lateUpdateSystem(
            system,
            system.timeStep() == System::TimeStep::Real ? deltaTime : gameTime);
      });
}

} // namespace sfs
