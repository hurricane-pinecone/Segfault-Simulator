#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/registry.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/game/gameObject.h"
#include "engine/runtime/input/input.h"
#include <SDL_render.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sfs
{

using SceneId = uint32_t;

class IQuadRenderer;
class TextRenderer;

// Engine-owned services a scene draws and loads assets through. Bundled into
// one value so they can be constructor-injected as a unit: every Scene holds
// them as references, valid for the scene's whole lifetime. The SceneManager
// builds this (and verifies the services exist) before constructing any scene.
struct SceneServices
{
  AssetStore& assetStore;
  IQuadRenderer& quadRenderer;
  TextRenderer& textRenderer;
};

class Scene
{
public:
  Scene(SceneId id, const SceneServices& services)
      : m_assetStore(services.assetStore),
        m_quadRenderer(services.quadRenderer),
        m_textRenderer(services.textRenderer), m_id(id),
        m_name("scene_" + std::to_string(id))
  {
  }

  Scene(SceneId id, const SceneServices& services, const std::string& name)
      : m_assetStore(services.assetStore),
        m_quadRenderer(services.quadRenderer),
        m_textRenderer(services.textRenderer), m_id(id), m_name(name)
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
  void destroyEntity(const Entity& entity);

  AssetStore& assetStore() const { return m_assetStore; }
  IQuadRenderer& quadRenderer() const { return m_quadRenderer; }
  TextRenderer& textRenderer() const { return m_textRenderer; }

  template <typename TObject, typename... TArgs>
  TObject& createObject(TArgs&&... args);

  template <typename TObject>
  TObject* tryFindObject() const;

  void destroyObject(GameObject* object);

  template <typename TSystem, typename... TArgs>
  TSystem& addSystem(TArgs&&... args);

  template <typename TSystem>
  void removeSystem();

  template <typename TSystem>
  bool hasSystem() const;

  template <typename TSystem>
  TSystem& getSystem() const;

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
  // Constructor-injected by SceneManager; references, so never null.
  IQuadRenderer& m_quadRenderer;
  TextRenderer& m_textRenderer;

  SceneId m_id = 0;
  std::string m_name = "";
  Registry m_registry;

  std::vector<std::unique_ptr<GameObject>> m_gameObjects;
};

template <typename TObject = GameObject, typename... TArgs>
TObject& Scene::createObject(TArgs&&... args)
{
  auto object = std::make_unique<TObject>(std::forward<TArgs>(args)...);
  TObject& ref = *object;

  m_gameObjects.push_back(std::move(object));
  ref.onCreate(*this);

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
