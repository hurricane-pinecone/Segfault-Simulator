#pragma once

#include "engine/core/components/contactReportComponent.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/hierarchyLinks.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/events/events.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/game/gameObject.h"
#include "engine/runtime/input/input.h"
#include "engine/runtime/systems/collision2D.h"
#include "engine/runtime/systems/transformSystem.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sfs
{

using SceneId = uint32_t;

class Game;
class IRenderer;
namespace reflect
{
struct TypeInfo;
}

// The renderer and asset store a scene draws and loads assets through,
// supplied by the active render backend. Bundled into one value so they can be
// constructor-injected as a unit. The asset store always exists; the renderer
// is null without one (a tool or test with no GPU). The SceneManager builds
// this before constructing any scene, and everything in it outlives every
// scene.
struct SceneBackends
{
  AssetStore& assetStore;
  IRenderer* renderer = nullptr;
};

// Every scene has a TransformSystem, added first, so the render phase opens
// with the transform pass (see TransformSystem).
class Scene
{
public:
  enum class Mode
  {
    EDITOR,
    GAME,
  };

  Scene(SceneId id, const SceneBackends& backends)
      : Scene(id, backends, "scene_" + std::to_string(id))
  {
  }

  Scene(SceneId id, const SceneBackends& backends, const std::string& name)
      : m_assetStore(backends.assetStore), m_renderer(backends.renderer),
        m_id(id), m_name(name)
  {
    addSystem<TransformSystem>();
  }

  virtual ~Scene() = default;

  /**
   * Gives the scene its one T, a type marked [[sfs::scene_data]], made from
   * `args`: Registry::addSceneData(). When it has one already, it logs an error
   * and keeps that one.
   * @return the scene's T
   */
  template <typename T, typename... TArgs>
  T& addSceneData(TArgs&&... args)
  {
    return m_registry.addSceneData<T>(std::forward<TArgs>(args)...);
  }

  /**
   * Gives the scene default-made data of a reflected scene data type:
   * addSceneData<T>() for a type known only at run time. One it has is kept,
   * with an error logged.
   * @param type the reflected type
   * @return the scene's one; null when the type isn't scene data
   */
  void* addSceneData(const reflect::TypeInfo& type);

  /**
   * Adds a service (docs/core/services), as Registry::addService() does: it
   * runs at the start of each step, before the systems, whenever its event
   * handles have something new.
   * @param args what its constructor takes
   * @return the service
   */
  template <typename TService, typename... TArgs>
  TService& addService(TArgs&&... args)
  {
    return m_registry.addService<TService>(std::forward<TArgs>(args)...);
  }

  template <typename TSystem, typename... TArgs>
  TSystem& addSystem(TArgs&&... args);

  // attachObject for an object made elsewhere (e.g. by a script factory, with
  // its fields already set): binds it to `entity`, then runs its onCreate.
  GameObject& adoptObject(std::unique_ptr<GameObject> object,
                          const Entity& entity);

  AssetStore& assetStore() const { return m_assetStore; }

  // Create a GameObject bound to an existing entity (e.g. one placed in the
  // editor): the object's entity() is already set when its onCreate runs.
  template <typename TObject>
  TObject& attachObject(const Entity& entity);

  /** Destroys all the scene's scene data, at once. */
  void clearSceneData() { m_registry.clearSceneData(); }

  Entity createEntity();

  template <typename TObject, typename... TArgs>
  TObject& createObject(TArgs&&... args);

  // Invoked inside the debug ImGui frame (debug builds only) so a scene can add
  // game-specific debug controls.
  void debugUI() { onDebugUI(); }

  // Destroy an entity and everything below it (at the next flush).
  void destroyBranch(const Entity& entity);

  // Destroy an entity (at the next flush). Its children become roots; use
  // destroyBranch to take them with it.
  void destroyEntity(const Entity& entity);

  /**
   * Destroy a game object and its entity (if it has one). It gets no more
   * calls; once the frame's updates, input or contacts are done (at once,
   * outside them) its onDestroy runs and it's deleted. Null, or an object
   * already destroyed, is ignored.
   */
  void destroyObject(GameObject* object);

  /**
   * Destroy every game object (their onDestroy runs), e.g. as the scene
   * ends; SceneManager does this before deleting a scene.
   */
  void destroyObjects();

  // Every living entity, in id order.
  std::vector<Entity> entities() { return m_registry.entities(); }

  /**
   * The scene's event queues (Registry::events()): send what one system tells
   * others with events().send(E{...}); read it through a reader made once
   * (an object's onCreate) and kept. Each frame's events start after the
   * flush, before the systems update.
   * @return the events
   */
  Events& events() { return m_registry.events(); }

  // Every entity with this name, parents before children.
  std::vector<Entity> findEntities(std::string_view name);

  // The first entity with this name (NameComponent), parents before children;
  // invalid if none. A path such as "Town/House/Door" starts from a root
  // entity's name and follows children's names down.
  Entity findEntity(std::string_view nameOrPath);

  // Every object of a type (or derived from it), in the order made. Objects
  // destroyed this frame aren't found.
  template <typename TObject>
  std::vector<TObject*> findObjects() const;

  // Apply pending entity creation/destruction now instead of at the start of
  // the next update (e.g. after an editor replaces the scene's contents).
  // Game objects on entities it destroys are destroyed too.
  void flushEntities();

  /**
   * Calls fn(Service&) for each service, in the order they were added (debug
   * tooling: enable/disable, inspect).
   * @param fn what to call
   */
  template <typename Fn>
  void forEachService(Fn&& fn)
  {
    m_registry.forEachService(std::forward<Fn>(fn));
  }

  // Iterate all systems (debug tooling: enable/disable, inspect).
  template <typename Fn>
  void forEachSystem(Fn&& fn)
  {
    m_registry.forEachSystem(std::forward<Fn>(fn));
  }

  // The game this scene runs in: quit, switch scenes, change the window. Like
  // the renderer, game() assumes there is one (there always is when a game or
  // the editor runs the scene); tryGame() is null in tools and tests.
  Game& game() const;

  /** Game seconds since play began: the time handed to the scene's systems
   * (Registry::gameTime()). */
  double gameTime() const { return m_registry.gameTime(); }

  Entity getEntity(Entity::EntityId id);

  template <typename TSystem>
  TSystem& getSystem() const;

  template <typename TSystem>
  bool hasSystem() const;

  /**
   * The flushed, living entities with a reflected component type, in id
   * order: query<T>() for a type known only at run time (tools that work
   * from reflection, as Unity's FindObjectsOfType(Type)).
   * @param type the component's reflected type
   * @return its query list, read-only; empty for a type that isn't a
   *         component
   */
  const std::vector<Entity>& holdersOf(const reflect::TypeInfo& type);

  SceneId id() const;
  const std::string& name() const;

  // Tell the game objects on both entities that a physics contact began or
  // ended (their onCollision/onTrigger Enter/Exit). Called by the scene's
  // physics, not by game code.
  void notifyContact(const Contact3D& contact, bool begun);
  void notifyContact(const Contact2D& contact, bool begun);

  /** Whether the game is paused (Registry::paused()). */
  bool paused() const { return m_registry.paused(); }

  /**
   * Every living entity, parents before children, roots and siblings in id
   * order (the order scenes save in), each with its parent's place in the
   * list: hierarchy::preorder() of the scene's registry. Rebuilt only after
   * the hierarchy or the set of entities changed, in place by the first call
   * after, so while iterating it, call nothing that may call preorder() (a
   * save, findEntity). Main thread only.
   * @return the index
   */
  const std::vector<HierarchyLinks::Placed>& preorder()
  {
    return m_registry.preorder();
  }

  // The frame's input: filterInput(), then each game object (without the
  // keys that input contexts outranking its own claim: see
  // InputContextComponent), then onProcessInput().
  void processInput(const sfs::Input& input);

  /**
   * The flushed, living entities that match these terms (components, one
   * Any<...> group, Excluding<...> groups), in id order: a view of the
   * registry's shared list, kept up to date, whose rows unpack the terms'
   * components (Registry::query()). To add or remove these component types
   * while going through it, go through a copy.
   * @return a view of the shared list, read-only
   */
  template <typename... TTerms>
  QueryView<TTerms...> query()
  {
    return m_registry.query<TTerms...>();
  }

  /** Destroys the scene's T, a type marked [[sfs::scene_data]], at once. */
  template <typename T>
  void removeSceneData()
  {
    m_registry.removeSceneData<T>();
  }

  /**
   * Destroys the scene's data of a reflected scene data type; nothing when
   * it has none or the type isn't scene data.
   * @param type the reflected type
   */
  void removeSceneData(const reflect::TypeInfo& type);

  template <typename TSystem>
  void removeSystem();

  /**
   * The render phase: each enabled system's render(), in the order they were
   * added (the transform pass first), then onRender(). Nothing in it should
   * write a TransformComponent: what's drawn reads the world matrices the
   * pass cached.
   */
  void render();

  // The backend's renderer. Asking for it where there's none (a tool without
  // a GPU) is a setup bug, so it's a fatal error (see fatalError()), like
  // getSystem; tryRenderer() returns null instead.
  IRenderer& renderer() const;

  /**
   * The root entities (those with no parent), in id order: hierarchy::roots()
   * of the scene's registry.
   * @return a range of Entity, read from the registry as it goes
   */
  hierarchy::RootRange roots() { return hierarchy::roots(m_registry); }

  /**
   * The scene's one T, a type marked [[sfs::scene_data]] (a match's scoreboard,
   * the weather), in constant time: Registry::sceneData().
   * @return it, or null when the scene has none
   */
  template <typename T>
  T* sceneData()
  {
    return m_registry.sceneData<T>();
  }

  /**
   * The scene's data of a reflected scene data type: sceneData<T>() for a type
   * known only at run time (the editor, the scene file).
   * @param type the reflected type
   * @return it, or null when the scene has none or the type isn't scene
   *         data
   */
  void* sceneDataOf(const reflect::TypeInfo& type);

  template <typename TObject>
  TObject* tryFindObject() const;

  Game* tryGame() const { return m_game; }

  // Null when the system isn't registered (for getSystem that's a fatal
  // error).
  template <typename TSystem>
  TSystem* tryGetSystem() const;

  IRenderer* tryRenderer() const { return m_renderer; }

  // A frame: the flush, the events' frame boundary (Events::update()), the
  // systems (in game time: see Registry::setTimeScale() and
  // System::TimeStep), then the game objects and onUpdate() (real time:
  // they're the player's, input and cameras), then the systems' lateUpdate().
  void update(double deltaTime);

protected:
  friend class SceneManager;

  // First, before any object sees the frame's input: a chance to take some
  // of it (a scene's UI, clicks on it).
  virtual void filterInput(Input& input){};
  virtual void onDebugUI(){};
  virtual void onEnter(){};
  virtual void onExit(){};
  virtual void onInit(){};
  virtual void onProcessInput(const Input& input){};
  virtual void onRender(){};
  virtual void onUpdate(double deltaTime){};

  AssetStore& m_assetStore;

private:
  // While game objects are being called (an update, input, contacts):
  // objects destroyed meanwhile are only marked, and deleted after.
  struct ObjectCalls
  {
    explicit ObjectCalls(Scene& scene) : scene(scene) { ++scene.m_objectCalls; }
    ~ObjectCalls() { --scene.m_objectCalls; }

    Scene& scene;
  };

  // Runs a new object's onCreate, as a call to it (so it isn't deleted under
  // itself), owning the event handles it makes there.
  void create(GameObject& object);
  // Run the destroyed objects' onDestroy and delete them, if no object calls
  // are under way.
  void deleteDestroyedObjects();
  // Mark the objects whose entities have been destroyed (and flushed).
  void destroyObjectsOfDeadEntities();
  Registry& registry();
  const Registry& registry() const;
  // update()'s systems, objects and onUpdate.
  void updateObjects(double deltaTime);

  // Constructor-injected by SceneManager; null where there's no renderer.
  IRenderer* m_renderer = nullptr;
  Game* m_game = nullptr; // set by SceneManager

  SceneId m_id = 0;
  std::string m_name = "";
  Registry m_registry;
  std::vector<std::unique_ptr<GameObject>> m_gameObjects;
  int m_objectCalls = 0;
  bool m_objectsDestroyed = false; // some in m_gameObjects are destroyed
};

template <typename TObject, typename... TArgs>
TObject& Scene::createObject(TArgs&&... args)
{
  auto object = std::make_unique<TObject>(std::forward<TArgs>(args)...);
  TObject& ref = *object;

  static_cast<GameObject&>(ref).m_scene = this;
  m_gameObjects.push_back(std::move(object));
  create(ref);
  return ref;
}

template <typename TObject>
TObject& Scene::attachObject(const Entity& entity)
{
  return static_cast<TObject&>(
      adoptObject(std::make_unique<TObject>(), entity));
}

template <typename TObject>
std::vector<TObject*> Scene::findObjects() const
{
  std::vector<TObject*> found;

  for (auto& object : m_gameObjects)
    if (auto casted = dynamic_cast<TObject*>(object.get());
        casted && !object->destroyed())
      found.push_back(casted);

  return found;
}

template <typename TObject>
TObject* Scene::tryFindObject() const
{
  for (auto& object : m_gameObjects)
  {
    if (auto casted = dynamic_cast<TObject*>(object.get());
        casted && !object->destroyed())
      return casted;
  }

  return nullptr;
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
TSystem* Scene::tryGetSystem() const
{
  return m_registry.tryGetSystem<TSystem>();
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
