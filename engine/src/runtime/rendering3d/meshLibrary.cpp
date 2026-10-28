#include "engine/runtime/rendering3d/meshLibrary.h"

#include "engine/core/logger/logger.h"
#include "engine/runtime/rendering3d/iRenderer3D.h"

#include "glm/glm/geometric.hpp"
#include "glm/glm/matrix.hpp"

#include <algorithm>
#include <cmath>

namespace sfs
{

MeshLibrary::MeshLibrary(IRenderer3D& renderer) : m_renderer(renderer) {}

MeshLibrary::~MeshLibrary()
{
  for (const auto& [id, entry] : m_meshes)
    if (!entry.missing)
      m_renderer.destroyMesh(MeshHandle{id});
  for (const auto& [name, material] : m_materials)
    m_renderer.destroyMaterial(material);
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

  Entry entry{mesh.bounds(), std::move(name), false, mesh.sections, {}, mesh.indices};
  entry.positions.reserve(mesh.vertices.size());
  for (const MeshVertex& vertex : mesh.vertices)
    entry.positions.push_back(vertex.position);
  m_meshes.emplace(handle.id, std::move(entry));
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

MeshHandle MeshLibrary::resolve(std::string_view name)
{
  // Anything already under this name (a stand-in included: entities hold its
  // handle) stays what the name means.
  if (const MeshHandle found = find(name); found.valid())
    return found;
  if (name.empty() || !m_provider.load)
    return {};
  return m_provider.load(*this, name);
}

std::vector<std::pair<std::string, std::string>> MeshLibrary::available() const
{
  std::vector<std::pair<std::string, std::string>> out;
  if (!m_provider.available)
    return out;
  for (auto& entry : m_provider.available())
  {
    const MeshHandle have = find(entry.first);
    if (!have.valid() || isMissing(have))
      out.push_back(std::move(entry));
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
  return out;
}

std::string MeshLibrary::label(std::string_view name) const
{
  if (m_provider.label)
    if (std::string text = m_provider.label(name); !text.empty())
      return text;
  return std::string(name);
}

std::vector<MeshSection> MeshLibrary::sections(MeshHandle mesh) const
{
  const auto it = m_meshes.find(mesh.id);
  if (it == m_meshes.end())
    return {};
  if (it->second.sections.empty())
    return {MeshSection{}};
  return it->second.sections;
}

std::optional<MeshLibrary::RayHit> MeshLibrary::raycast(MeshHandle mesh,
                                                       const glm::mat4& world,
                                                       const Ray3D& ray) const
{
  const auto it = m_meshes.find(mesh.id);
  if (it == m_meshes.end())
    return std::nullopt;
  const Entry& entry = it->second;

  // Into the mesh's space; the direction isn't renormalised, so distances
  // along it stay the world ray's.
  const glm::mat4 toLocal = glm::inverse(world);
  const glm::vec3 origin(toLocal * glm::vec4(ray.origin, 1.0f));
  const glm::vec3 direction(toLocal * glm::vec4(ray.direction, 0.0f));

  std::optional<RayHit> best;
  std::size_t bestIndex = 0;
  for (std::size_t i = 0; i + 2 < entry.indices.size(); i += 3)
  {
    if (entry.indices[i + 2] >= entry.positions.size() || entry.indices[i] >= entry.positions.size() ||
        entry.indices[i + 1] >= entry.positions.size())
      continue;
    // Moller-Trumbore.
    const glm::vec3& a = entry.positions[entry.indices[i]];
    const glm::vec3 edge1 = entry.positions[entry.indices[i + 1]] - a;
    const glm::vec3 edge2 = entry.positions[entry.indices[i + 2]] - a;
    const glm::vec3 p = glm::cross(direction, edge2);
    const float determinant = glm::dot(edge1, p);
    if (std::abs(determinant) < 1.0e-12f)
      continue;
    const float inverse = 1.0f / determinant;
    const glm::vec3 t = origin - a;
    const float u = glm::dot(t, p) * inverse;
    if (u < 0.0f || u > 1.0f)
      continue;
    const glm::vec3 q = glm::cross(t, edge1);
    const float v = glm::dot(direction, q) * inverse;
    if (v < 0.0f || u + v > 1.0f)
      continue;
    const float distance = glm::dot(edge2, q) * inverse;
    if (distance > 0.0f && (!best || distance < best->distance))
    {
      best = RayHit{distance, 0};
      bestIndex = i;
    }
  }
  if (best)
    for (std::size_t s = 0; s < entry.sections.size(); ++s)
      if (bestIndex >= entry.sections[s].firstIndex &&
          bestIndex < std::size_t(entry.sections[s].firstIndex) + entry.sections[s].indexCount)
        best->section = s;
  return best;
}

MaterialHandle MeshLibrary::material(std::string_view name)
{
  if (name.empty())
    return {};
  const std::string key(name);
  if (const auto it = m_materials.find(key); it != m_materials.end())
    return it->second;
  const std::shared_ptr<const MeshMaterial> data =
      m_provider.loadMaterial ? m_provider.loadMaterial(name) : nullptr;
  const MaterialHandle handle = data ? m_renderer.createMaterial(*data) : MaterialHandle{};
  m_materials.emplace(key, handle);
  return handle;
}

void MeshLibrary::reloadMaterial(std::string_view name)
{
  const auto it = m_materials.find(std::string(name));
  if (it == m_materials.end())
    return;
  const std::shared_ptr<const MeshMaterial> data =
      m_provider.loadMaterial ? m_provider.loadMaterial(name) : nullptr;
  if (it->second.valid() && data)
    m_renderer.updateMaterial(it->second, *data);
  else
  {
    // Gone, or only now readable: ask afresh next time.
    m_renderer.destroyMaterial(it->second);
    m_materials.erase(it);
  }
}

std::vector<std::pair<std::string, std::string>> MeshLibrary::availableMaterials() const
{
  std::vector<std::pair<std::string, std::string>> out;
  if (m_provider.materials)
    out = m_provider.materials();
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
  return out;
}

std::vector<std::pair<std::string, std::string>> MeshLibrary::availableTextures() const
{
  std::vector<std::pair<std::string, std::string>> out;
  if (m_provider.textures)
    out = m_provider.textures();
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
  return out;
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
