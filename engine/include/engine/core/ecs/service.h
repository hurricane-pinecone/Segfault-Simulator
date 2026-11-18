#pragma once

#include "engine/core/ecs/entityDeclaration.h"
#include "engine/core/ecs/facilityAccess.h"
#include "engine/core/ecs/moduleSettings.h"
#include "engine/core/events/eventConsumer.h"
#include "engine/core/events/eventVerbs.h"

#include <vector>

// The bodies of the templates that need the complete Registry are in
// service.inl, which registry.h includes at its end.
namespace sfs
{

class Registry;

/**
 * A service (docs/core/services): logic that runs only when its events bring
 * it something. It has no update and never looks through the registry: it
 * sends events, and reacts in its handlers to the events its consumers and
 * requesters receive, reading and changing the components of the entities
 * those events hand it and the scene's data (it may be a field's one
 * writer, its own scene data included), and it may create and destroy
 * entities. Its handlers may use the engine's facilities (physics(),
 * navigation(), prefabs(), ...: docs/runtime/facilities): a raycast, a path
 * order, a spawn. An entity a facility query gives (a raycast's hit), and
 * what it leads to, it may read, but it changes only the entities its events
 * hand it and those it makes. It makes its handles in create(), each consumer
 * with a handler, and keeps them as members. Its host runs it at the start of
 * each step, before the systems, when one of its handles has something new.
 */
class Service : public EventVerbs
{
public:
  Service() = default;
  ~Service() override = default;

  /** Debug settings, as System::settings(). Default: none. */
  virtual std::vector<ModuleSetting> settings() { return {}; }

protected:
  /**
   * Gives the scene its one T, a type marked [[sfs::scene_data]]
   * (Registry::addSceneData()): a service may make its own state so. When the
   * scene has one already, it logs an error and keeps that one.
   * @return the scene's T; null if the service isn't in a registry
   */
  template <typename T, typename... TArgs>
  T* addSceneData(TArgs&&... args);

  /** The engine's animation clips (facility::Animation). */
  inline facility::Animation animation() const;
  /** The scene's asset loading (facility::Assets). */
  inline facility::Assets assets() const;
  /** The scene's cameras (facility::Cameras). */
  inline facility::Cameras cameras() const;

  /**
   * A consumer of TEvent, made in create(): a builder that becomes an
   * EventConsumer<TEvent> once given a handler (onEvent(), onEvents(),
   * onLatest(), onLatestEvents()), since a service has no update to read it
   * in. Its handlers may read() other consumers of the service.
   * @return the consumer's builder
   */
  template <typename TEvent>
  EventConsumerBuilder<TEvent> consumer()
  {
    return EventConsumerBuilder<TEvent>(EventVerbs::consumer<TEvent>());
  }

  /**
   * Sets the service up, once, as it's added, with its events set: makes its
   * consumers (each with its handler) and requesters, kept as members, and
   * builds what it builds once (a scheduler it owns). A service makes no
   * handles anywhere else.
   */
  virtual void create() = 0;

  /**
   * Creates an entity (Registry::createEntity()): it can be given components
   * at once, and joins the lists at the registry's next flush, as one a
   * system creates.
   * @return the new entity; invalid if the service isn't in a registry
   */
  Entity createEntity();

  /**
   * Debug drawing, in development builds only (facility::DebugDraw). A
   * service has no update, so a line it draws shows for one frame.
   */
  inline facility::DebugDraw debugDraw() const;

  /**
   * Destroys an entity at the registry's next flush
   * (Registry::destroyEntity()); a handle that isn't valid is ignored.
   * @param entity the entity
   */
  void destroyEntity(const Entity& entity);

  /** The scene's rendering (facility::Graphics). */
  inline facility::Graphics graphics() const;
  /** The scene's meshes (facility::Meshes). */
  inline facility::Meshes meshes() const;
  /** The scene's navigation (facility::Navigation). */
  inline facility::Navigation navigation() const;
  /** The scene's 3D physics (facility::Physics3D). */
  inline facility::Physics3D physics() const;
  /** Prefab copies made as the game runs (facility::Prefabs). */
  inline facility::Prefabs prefabs() const;

  /**
   * Destroys the scene's T, a type marked [[sfs::scene_data]]
   * (Registry::removeSceneData()); nothing if it has none.
   */
  template <typename T>
  void removeSceneData();

  /**
   * The scene's one T, a type marked [[sfs::scene_data]] (a clock, the weather,
   * the service's own state): Registry::sceneData().
   * @return it, or null when the scene has none
   */
  template <typename T>
  T* sceneData();

private:
  friend class Registry;

  // Its registry, for the scene data, the facilities, createEntity() and
  // destroyEntity() alone: private, so a service's own code never reaches it.
  // Set as it's added.
  Registry* m_registry = nullptr;
};

} // namespace sfs
