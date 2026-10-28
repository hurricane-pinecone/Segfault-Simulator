#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/registry.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/game/gameObject.h"
#include "engine/runtime/input/input.h"
#include <SDL_render.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sfs
{

using SceneId = uint32_t;

class Game;
class IQuadRenderer;
class IRenderer3D;
class TextRenderer;

// Engine-owned services a scene draws and loads assets through, supplied by the
// active render backend. Bundled into one value so they can be
// constructor-injected as a unit. The asset store always exists; the renderers
// are whichever the backend actually has (an OpenGL backend draws 2D quads and
// text, a WebGPU backend draws 3D), so each is null when absent. The
// SceneManager builds this before constructing any scene, and everything in it
// outlives every scene.
struct SceneServices
{
  AssetStore& assetStore;
  IQuadRenderer* quadRenderer = nullptr;
  TextRenderer* textRenderer = nullptr;
  IRenderer3D* renderer3D = nullptr;
};

class Scene
{
public:
  Scene(SceneId id, const SceneServices& services)
      : m_assetStore(services.assetStore),
        m_quadRenderer(services.quadRenderer),
        m_textRenderer(services.textRenderer),
        m_renderer3D(services.renderer3D), m_id(id),
        m_name("scene_" + std::to_string(id))
  {
  }

  Scene(SceneId id, const SceneServices& services, const std::string& name)
      : m_assetStore(services.assetStore),
        m_quadRenderer(services.quadRenderer),
        m_textRenderer(services.textRenderer),
        m_renderer3D(services.renderer3D), m_id(id), m_name(name)
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

  // Invoked inside the debug ImGui frame (debug builds only) so a scene can add
  // game-specific debug controls.
  void debugUI() { onDebugUI(); }

  Entity createEntity();
  Entity getEntity(Entity::EntityId id);

  // Destroy an entity (at the next flush). Its children become roots; use
  // destroyBranch to take them with it.
  void destroyEntity(const Entity& entity);

  // Destroy an entity and everything below it (at the next flush).
  void destroyBranch(const Entity& entity);

  // The first entity with this name (NameComponent), parents before children;
  // invalid if none. A path such as "Farm/Barn/Door" starts from a root
  // entity's name and follows children's names down.
  Entity findEntity(std::string_view nameOrPath);

  // Every entity with this name, parents before children.
  std::vector<Entity> findEntities(std::string_view name);

  // Every living entity, in id order (e.g. for an editor's hierarchy).
  std::vector<Entity> entities() { return m_registry.entities(); }

  // Apply pending entity creation/destruction now instead of at the start of
  // the next update (e.g. after an editor replaces the scene's contents).
  void flushEntities() { m_registry.flushEntities(); }

  AssetStore& assetStore() const { return m_assetStore; }
  // The backend's renderers. Each accessor assumes the backend provides that
  // renderer: asking for one it lacks is a setup bug (e.g. a 3D scene on the
  // OpenGL backend), so it logs and aborts, like getSystem. The try- forms
  // return null instead, for code that adapts to the backend.
  IQuadRenderer& quadRenderer() const;
  TextRenderer& textRenderer() const;
  IRenderer3D& renderer3D() const;
  // The game this scene runs in: quit, switch scenes, change the window. Like
  // the renderers, game() assumes there is one (there always is when a game or
  // the editor runs the scene); tryGame() is null in tools and tests.
  Game& game() const;
  Game* tryGame() const { return m_game; }

  IQuadRenderer* tryQuadRenderer() const { return m_quadRenderer; }
  TextRenderer* tryTextRenderer() const { return m_textRenderer; }
  IRenderer3D* tryRenderer3D() const { return m_renderer3D; }

  template <typename TObject, typename... TArgs>
  TObject& createObject(TArgs&&... args);

  // Create a GameObject bound to an existing entity (e.g. one placed in the
  // editor): the object's entity() is already set when its onCreate runs.
  template <typename TObject>
  TObject& attachObject(const Entity& entity);

  // attachObject for an object made elsewhere (e.g. by a script factory, with
  // its fields already set): binds it to `entity`, then runs its onCreate.
  GameObject& adoptObject(std::unique_ptr<GameObject> object, const Entity& entity);

  template <typename TObject>
  TObject* tryFindObject() const;

  void destroyObject(GameObject* object);

  // Tell the game objects on both entities that a physics contact began or
  // ended (their onCollision/onTrigger Enter/Exit). Called by the scene's
  // physics, not by game code.
  void notifyContact(const Contact3D& contact, bool begun);

  template <typename TSystem, typename... TArgs>
  TSystem& addSystem(TArgs&&... args);

  template <typename TSystem>
  void removeSystem();

  template <typename TSystem>
  bool hasSystem() const;

  template <typename TSystem>
  TSystem& getSystem() const;

  // Null when the system isn't registered (getSystem aborts instead).
  template <typename TSystem>
  TSystem* tryGetSystem() const;

  // Iterate all systems (debug tooling: enable/disable, inspect).
  template <typename Fn>
  void forEachSystem(Fn&& fn)
  {
    m_registry.forEachSystem(std::forward<Fn>(fn));
  }

private:
  Registry& registry();
  const Registry& registry() const;

protected:
  virtual void onInit(){};
  virtual void onEnter(){};
  virtual void onExit(){};
  virtual void onUpdate(double deltaTime){};
  virtual void onRender(){};
  virtual void onProcessInput(const Input& input){};
  virtual void onDebugUI(){};

  friend class SceneManager;

protected:
  AssetStore& m_assetStore;

private:
  // Constructor-injected by SceneManager; null where the backend has none.
  IQuadRenderer* m_quadRenderer = nullptr;
  TextRenderer* m_textRenderer = nullptr;
  IRenderer3D* m_renderer3D = nullptr;
  Game* m_game = nullptr; // set by SceneManager

  SceneId m_id = 0;
  std::string m_name = "";
  Registry m_registry;

  std::vector<std::unique_ptr<GameObject>> m_gameObjects;
};

template <typename TObject, typename... TArgs>
TObject& Scene::createObject(TArgs&&... args)
{
  auto object = std::make_unique<TObject>(std::forward<TArgs>(args)...);
  TObject& ref = *object;

  static_cast<GameObject&>(ref).m_scene = this;
  m_gameObjects.push_back(std::move(object));
  // Through the base, where Scene is a friend: scripts override onCreate as
  // protected, which would otherwise be inaccessible here.
  static_cast<GameObject&>(ref).onCreate(*this);

  return ref;
}

template <typename TObject>
TObject& Scene::attachObject(const Entity& entity)
{
  auto object = std::make_unique<TObject>();
  TObject& ref = *object;
  static_cast<GameObject&>(ref).m_entity = entity;
  static_cast<GameObject&>(ref).m_scene = this;

  m_gameObjects.push_back(std::move(object));
  // Through the base, where Scene is a friend: scripts override onCreate as
  // protected, which would otherwise be inaccessible here.
  static_cast<GameObject&>(ref).onCreate(*this);

  return ref;
}

template <typename TObject>
TObject* Scene::tryFindObject() const
{
  for (auto& object : m_gameObjects)
  {
    if (auto casted = dynamic_cast<TObject*>(object.get()))
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
