#include "engine/runtime/rendering3d/meshLibrary.h"

#include "engine/core/logger/logger.h"
#include "engine/runtime/rendering3d/iRenderer3D.h"

#include <algorithm>

namespace sfs
{

MeshLibrary::MeshLibrary(IRenderer3D& renderer) : m_renderer(renderer) {}

MeshLibrary::~MeshLibrary()
{
  for (const auto& [id, entry] : m_meshes)
    if (!entry.missing)
      m_renderer.destroyMesh(MeshHandle{id});
}

MeshHandle MeshLibrary::add(const MeshData& mesh, std::string name)
{
  if (mesh.empty())
  {
    LOG_ERROR("MeshLibrary::add: mesh has no triangles");
    return {};
  }
  if (!name.empty() && find(name).valid())
  {
    LOG_ERROR("MeshLibrary::add: a mesh named '" + name + "' already exists");
    return {};
  }

  const MeshHandle handle = m_renderer.createMesh(mesh);
  if (!handle.valid())
  {
    LOG_ERROR("MeshLibrary::add: renderer failed to upload mesh");
    return {};
  }

  m_meshes.emplace(handle.id, Entry{mesh.bounds(), std::move(name)});
  return handle;
}

void MeshLibrary::remove(MeshHandle mesh)
{
  const auto it = m_meshes.find(mesh.id);
  if (it == m_meshes.end())
    return;
  const bool missing = it->second.missing;
  m_meshes.erase(it);
  if (!missing)
    m_renderer.destroyMesh(mesh);
}

MeshHandle MeshLibrary::missing(std::string_view name)
{
  if (const MeshHandle existing = find(name); existing.valid())
    return existing;
  const MeshHandle handle{m_nextMissingId++};
  m_meshes.emplace(handle.id, Entry{Aabb{}, std::string(name), true});
  return handle;
}

bool MeshLibrary::isMissing(MeshHandle mesh) const
{
  const auto it = m_meshes.find(mesh.id);
  return it != m_meshes.end() && it->second.missing;
}

const Aabb* MeshLibrary::bounds(MeshHandle mesh) const
{
  const auto it = m_meshes.find(mesh.id);
  return it != m_meshes.end() && !it->second.missing ? &it->second.bounds : nullptr;
}

std::string_view MeshLibrary::nameOf(MeshHandle mesh) const
{
  const auto it = m_meshes.find(mesh.id);
  return it != m_meshes.end() ? std::string_view(it->second.name) : std::string_view();
}

MeshHandle MeshLibrary::find(std::string_view name) const
{
  if (name.empty())
    return {};
  for (const auto& [id, entry] : m_meshes)
    if (entry.name == name)
      return MeshHandle{id};
  return {};
}

std::vector<std::pair<MeshHandle, std::string>> MeshLibrary::named() const
{
  std::vector<std::pair<MeshHandle, std::string>> out;
  for (const auto& [id, entry] : m_meshes)
    if (!entry.name.empty() && !entry.missing)
      out.emplace_back(MeshHandle{id}, entry.name);
  std::sort(out.begin(), out.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });
  return out;
}

} // namespace sfs
