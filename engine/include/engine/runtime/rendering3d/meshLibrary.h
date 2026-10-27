#pragma once

#include "engine/core/rendering/3d/aabb.h"
#include "engine/core/rendering/3d/mesh.h"

#include <cstddef>
#include <unordered_map>

namespace sfs
{

class IRenderer3D;

/**
 * The meshes a scene has uploaded: wraps IRenderer3D's create/destroy and keeps
 * each mesh's local bounds for culling. Owns its meshes, so they are freed with
 * the library (i.e. with the scene's render system).
 */
class MeshLibrary
{
public:
  explicit MeshLibrary(IRenderer3D& renderer);
  ~MeshLibrary();

  MeshLibrary(const MeshLibrary&) = delete;
  MeshLibrary& operator=(const MeshLibrary&) = delete;

  /**
   * Upload a mesh.
   *
   * @return its handle, or an invalid handle (logged) if the mesh is empty or
   * the upload failed.
   */
  MeshHandle add(const MeshData& mesh);

  /** Free a mesh this library added. Other handles are ignored. */
  void remove(MeshHandle mesh);

  /** @return the mesh's local-space bounds, or nullptr if it isn't ours. */
  const Aabb* bounds(MeshHandle mesh) const;

  std::size_t size() const { return m_bounds.size(); }

private:
  IRenderer3D& m_renderer;
  std::unordered_map<uint32_t, Aabb> m_bounds;
};

} // namespace sfs
