#include "engine/runtime/rendering3d/scene3D.h"

#include "engine/core/rendering/3d/meshPrimitives.h"
#include "engine/runtime/rendering3d/modules/meshModule.h"
#include "engine/runtime/rendering3d/renderSystem3D.h"
#include "engine/runtime/scripting/scriptRegistry.h"

namespace sfs
{

namespace
{
// Neutral grey so a primitive reads as "unstyled" until given a tint.
constexpr glm::vec3 kPrimitiveColor{0.78f, 0.78f, 0.80f};
} // namespace

void Scene3D::onInit()
{
  m_render = &addSystem<RenderSystem3D>(renderer3D());
  m_render->withModule<MeshModule>();

  MeshLibrary& library = m_render->meshes();
  // Named, so scenes that use them save and load.
  m_cube = library.add(meshes::box({1.0f, 1.0f, 1.0f}, kPrimitiveColor), "builtin:cube");
  m_sphere = library.add(meshes::sphere(0.5f, 24, 16, kPrimitiveColor), "builtin:sphere");
  m_plane = library.add(meshes::plane({10.0f, 10.0f}, kPrimitiveColor), "builtin:plane");
  m_cylinder = library.add(meshes::cylinder(0.5f, 1.0f, 24, kPrimitiveColor), "builtin:cylinder");

  // Meshes the game builds in code, so scenes can name them like the above.
  for (const ScriptType& script : scripts().types())
    if (script.kind == ScriptType::Kind::MeshSource && script.buildMeshes)
      script.buildMeshes(library);
}

MeshHandle Scene3D::builtinMesh(BuiltinMesh mesh) const
{
  switch (mesh)
  {
  case BuiltinMesh::Cube:
    return m_cube;
  case BuiltinMesh::Sphere:
    return m_sphere;
  case BuiltinMesh::Plane:
    return m_plane;
  case BuiltinMesh::Cylinder:
    return m_cylinder;
  }
  return {};
}

} // namespace sfs
