#include "scene/editorScene.h"

#include "engine/core/components/cameraComponent.h"
#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/components/nameComponent.h"
#include "engine/core/components/pointLightComponent.h"
#include "engine/core/components/spriteComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/runtime/rendering/renderSystem.h"

void EditorScene::onInit()
{
  sfs::StandardScene::onInit();

  sfs::Camera& camera = renderSystem().camera();
  camera.yaw = 0.9f;
  camera.pitch = 0.55f;
  camera.distance = 14.0f;

  resetToDefault();
}

sfs::Entity EditorScene::createEmpty(const std::string& name)
{
  sfs::Entity entity = createEntity();
  entity.addComponent<sfs::NameComponent>(name);
  entity.addComponent<sfs::TransformComponent>();
  return entity;
}

sfs::Entity EditorScene::createPrimitive(Primitive primitive,
                                         const std::string& name)
{
  sfs::Entity entity = createEmpty(name);
  entity.addComponent<sfs::MeshRendererComponent>(
      sfs::MeshRendererComponent{builtinMesh(primitive)});
  return entity;
}

sfs::Entity EditorScene::createPointLight(const std::string& name)
{
  sfs::Entity entity = createEmpty(name);
  entity.addComponent<sfs::PointLightComponent>();
  return entity;
}

sfs::Entity EditorScene::createSprite(const std::string& name)
{
  sfs::Entity entity = createEmpty(name);
  entity.addComponent<sfs::SpriteComponent>();
  return entity;
}

sfs::Entity EditorScene::createCamera(const std::string& name,
                                      const sfs::Camera& view,
                                      const sfs::Space2D* space)
{
  sfs::Entity entity = createEmpty(name);
  if (space)
  {
    entity.getComponent<sfs::TransformComponent>().setPosition2D(
        space->unplace(glm::vec2(view.target), 0.0f));
    sfs::CameraComponent camera;
    camera.orthographic = true;
    camera.orthoHeight = view.orthoHeight;
    camera.farPlane = 100000.0f;
    entity.addComponent<sfs::CameraComponent>(camera);
    return entity;
  }
  entity.getComponent<sfs::TransformComponent>() =
      sfs::TransformComponent(view.position(), view.orientation());
  sfs::CameraComponent camera;
  camera.focusDistance = view.distance;
  entity.addComponent<sfs::CameraComponent>(camera);
  return entity;
}

void EditorScene::destroyBranch(const sfs::Entity& entity)
{
  sfs::Scene::destroyBranch(entity);
  flushEntities();
}

void EditorScene::clear()
{
  for (const sfs::Entity& entity : entities())
    destroyEntity(entity);
  flushEntities();
}

void EditorScene::resetToDefault()
{
  clear();
  gameSystems().clear();

  sfs::Entity ground = createPrimitive(Primitive::Plane, "Ground");
  auto& groundRenderer = ground.getComponent<sfs::MeshRendererComponent>();
  groundRenderer.tint = {0.55f, 0.70f, 0.45f, 1.0f};
  groundRenderer.castsShadow = false;

  createPrimitive(Primitive::Cube, "Cube");

  sfs::Entity light = createPointLight("Point Light");
  light.getComponent<sfs::TransformComponent>().position = {2.0f, -2.0f, 2.0f};
  flushEntities();
}
