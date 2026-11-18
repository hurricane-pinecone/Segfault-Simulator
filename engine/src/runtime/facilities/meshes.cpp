#include "engine/runtime/facilities/meshes.h"

#include "absentBackend.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/rendering/aabb.h"
#include "engine/core/rendering/camera.h"
#include "engine/core/rendering/mesh.h"
#include "engine/runtime/rendering/meshLibrary.h"
#include "engine/runtime/rendering/picking.h"
#include "engine/runtime/rendering/renderSystem.h"

#include <glm/glm/ext/matrix_float4x4.hpp>
#include <glm/glm/ext/vector_float3.hpp>

#include <atomic>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace sfs::facility
{

namespace
{
std::atomic<bool> s_warned{false};
} // namespace

MeshHandle Meshes::add(const MeshData& mesh, std::string name) const
{
  MeshLibrary* meshes = backend();
  return meshes ? meshes->add(mesh, std::move(name)) : MeshHandle{};
}

MeshLibrary* Meshes::backend() const
{
  if (!m_render)
  {
    warnAbsent(s_warned, "meshes", "RenderSystem");
    return nullptr;
  }

  return &m_render->meshes();
}

const Aabb* Meshes::bounds(MeshHandle mesh) const
{
  const MeshLibrary* meshes = backend();
  return meshes ? meshes->bounds(mesh) : nullptr;
}

MeshHandle Meshes::find(std::string_view name) const
{
  const MeshLibrary* meshes = backend();
  return meshes ? meshes->find(name) : MeshHandle{};
}

Meshes Meshes::in(Registry* registry)
{
  RenderSystem* render =
      registry ? registry->tryGetSystem<RenderSystem>() : nullptr;
  return Meshes(render, registry);
}

std::string_view Meshes::nameOf(MeshHandle mesh) const
{
  const MeshLibrary* meshes = backend();
  return meshes ? meshes->nameOf(mesh) : std::string_view{};
}

std::optional<PickHit> Meshes::pickEntity(const Ray& ray) const
{
  const MeshLibrary* meshes = backend();
  return meshes ? sfs::pickEntity(*m_registry, *meshes, ray, m_render)
                : std::nullopt;
}

std::optional<SurfaceHit> Meshes::pickSurface(const Ray& ray) const
{
  const MeshLibrary* meshes = backend();
  return meshes ? sfs::pickSurface(*m_registry, *meshes, ray) : std::nullopt;
}

std::span<const glm::vec3> Meshes::positions(MeshHandle mesh) const
{
  const MeshLibrary* meshes = backend();
  return meshes ? meshes->positions(mesh) : std::span<const glm::vec3>{};
}

std::optional<MeshLibrary::RayHit>
Meshes::raycast(MeshHandle mesh, const glm::mat4& world, const Ray& ray) const
{
  const MeshLibrary* meshes = backend();
  return meshes ? meshes->raycast(mesh, world, ray) : std::nullopt;
}

void Meshes::remove(MeshHandle mesh) const
{
  if (MeshLibrary* meshes = backend())
    meshes->remove(mesh);
}

MeshHandle Meshes::resolve(std::string_view name) const
{
  MeshLibrary* meshes = backend();
  return meshes ? meshes->resolve(name) : MeshHandle{};
}

MaterialHandle Meshes::setMaterial(std::string_view name,
                                   const MeshMaterial& material) const
{
  MeshLibrary* meshes = backend();
  return meshes ? meshes->setMaterial(name, material) : MaterialHandle{};
}

} // namespace sfs::facility
