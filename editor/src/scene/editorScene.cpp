#include "scene/editorScene.h"

#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/components/nameComponent.h"
#include "engine/core/components/pointLight3DComponent.h"
#include "engine/core/components/transform3DComponent.h"
#include "engine/core/rendering/3d/meshPrimitives.h"
#include "engine/runtime/rendering3d/modules/meshModule.h"
#include "engine/runtime/rendering3d/renderSystem3D.h"

namespace meshes = sfs::meshes;

namespace
{
// Neutral grey so a primitive reads as "unstyled" until given a tint.
constexpr glm::vec3 kPrimitiveColor{0.78f, 0.78f, 0.80f};
} // namespace

void EditorScene::onInit()
{
  m_render = &addSystem<sfs::RenderSystem3D>(renderer3D());
  m_render->withModule<sfs::MeshModule>();

  sfs::MeshLibrary& library = m_render->meshes();
  m_cube = library.add(meshes::box({1.0f, 1.0f, 1.0f}, kPrimitiveColor));
  m_sphere = library.add(meshes::sphere(0.5f, 24, 16, kPrimitiveColor));
  m_plane = library.add(meshes::plane({10.0f, 10.0f}, kPrimitiveColor));
  m_cylinder = library.add(meshes::cylinder(0.5f, 1.0f, 24, kPrimitiveColor));

  sfs::Camera3D& camera = m_render->camera();
  camera.yaw = 0.9f;
  camera.pitch = 0.55f;
  camera.distance = 14.0f;

  populateDefault();
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
  sfs::MeshHandle mesh;
  switch (primitive)
  {
  case Primitive::Cube:
    mesh = m_cube;
    break;
  case Primitive::Sphere:
    mesh = m_sphere;
    break;
  case Primitive::Plane:
    mesh = m_plane;
    break;
  case Primitive::Cylinder:
    mesh = m_cylinder;
    break;
  }

  sfs::Entity entity = createEmpty(name);
  entity.addComponent<sfs::MeshRendererComponent>(sfs::MeshRendererComponent{mesh});
  return entity;
}

sfs::Entity EditorScene::createPointLight(const std::string& name)
{
  sfs::Entity entity = createEmpty(name);
  entity.addComponent<sfs::PointLight3DComponent>();
  return entity;
}

void EditorScene::populateDefault()
{
  sfs::Entity ground = createPrimitive(Primitive::Plane, "Ground");
  auto& groundRenderer = ground.getComponent<sfs::MeshRendererComponent>();
  groundRenderer.tint = {0.55f, 0.70f, 0.45f, 1.0f};
  groundRenderer.castsShadow = false;

  createPrimitive(Primitive::Cube, "Cube");

  sfs::Entity light = createPointLight("Point Light");
  light.getComponent<sfs::Transform3DComponent>().position = {2.0f, -2.0f, 2.0f};
}
