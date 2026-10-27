#include "engine/runtime/rendering3d/meshLibrary.h"

#include "engine/core/logger/logger.h"
#include "engine/runtime/rendering3d/iRenderer3D.h"

namespace sfs
{

MeshLibrary::MeshLibrary(IRenderer3D& renderer) : m_renderer(renderer) {}

MeshLibrary::~MeshLibrary()
{
  for (const auto& [id, bounds] : m_bounds)
    m_renderer.destroyMesh(MeshHandle{id});
}

MeshHandle MeshLibrary::add(const MeshData& mesh)
{
  if (mesh.empty())
  {
    LOG_ERROR("MeshLibrary::add: mesh has no triangles");
    return {};
  }

  const MeshHandle handle = m_renderer.createMesh(mesh);
  if (!handle.valid())
  {
    LOG_ERROR("MeshLibrary::add: renderer failed to upload mesh");
    return {};
  }

  m_bounds.emplace(handle.id, mesh.bounds());
  return handle;
}

void MeshLibrary::remove(MeshHandle mesh)
{
  if (m_bounds.erase(mesh.id) > 0)
    m_renderer.destroyMesh(mesh);
}

const Aabb* MeshLibrary::bounds(MeshHandle mesh) const
{
  const auto it = m_bounds.find(mesh.id);
  return it != m_bounds.end() ? &it->second : nullptr;
}

} // namespace sfs
