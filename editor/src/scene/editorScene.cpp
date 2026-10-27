#include "scene/editorScene.h"

#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/components/nameComponent.h"
#include "engine/core/components/pointLight3DComponent.h"
#include "engine/core/components/transform3DComponent.h"
#include "engine/runtime/rendering3d/renderSystem3D.h"

void EditorScene::onInit()
{
  sfs::Scene3D::onInit();

  sfs::Camera3D& camera = renderSystem().camera();
  camera.yaw = 0.9f;
  camera.pitch = 0.55f;
  camera.distance = 14.0f;

  resetToDefault();
}

sfs::Entity EditorScene::createEmpty(const std::string& name)
{
  sfs::Entity entity = createEntity();
  entity.addComponent<sfs::NameComponent>(name);
  entity.addComponent<sfs::Transform3DComponent>();
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
  entity.addComponent<sfs::PointLight3DComponent>();
  return entity;
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
  light.getComponent<sfs::Transform3DComponent>().position = {2.0f, -2.0f, 2.0f};
  flushEntities();
}
