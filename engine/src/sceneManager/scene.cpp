
#include "engine/input/input.h"
#include "engine/systems/isometricRenderSystem.h"
#include <engine/ecs/registry.h>
#include <engine/sceneManager/scene.h>
#include <engine/systems/renderSystem.h>

namespace sfs
{

SceneId Scene::id() const { return m_id; }
const std::string& Scene::name() const { return m_name; }

Registry& Scene::registry() { return m_registry; }
const Registry& Scene::registry() const { return m_registry; }

Entity Scene::createEntity() { return m_registry.createEntity(); }
Entity Scene::getEntity(int id) { return m_registry.getEntity(id); }
void Scene::destroyEntity(int id) { m_registry.destroyEntity(id); }

void Scene::update(double deltaTime)
{
  m_registry.update(deltaTime);
  onUpdate(deltaTime);
}
void Scene::processInput(const Input& input) { onProcessInput(input); }
void Scene::render()
{
  if (registry().hasSystem<IsometricRenderSystem>())
  {
    registry().getSystem<IsometricRenderSystem>().render();
  }
  // if (registry().hasSystem<RenderSystem>())
  // {
  //   registry().getSystem<RenderSystem>().render(renderer);
  // }

  onRender();
  onPostRender();
}

} // namespace sfs
