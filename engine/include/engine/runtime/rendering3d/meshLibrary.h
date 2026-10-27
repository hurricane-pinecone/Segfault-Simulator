#pragma once

#include "engine/core/rendering/3d/aabb.h"
#include "engine/core/rendering/3d/mesh.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

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
   * Upload a mesh, optionally under a name. A name is how saved scenes refer
   * to the mesh (handles are only valid for one run), so name any mesh an
   * entity in a saved scene uses.
   *
   * @return its handle, or an invalid handle (logged) if the mesh is empty,
   * the name is taken, or the upload failed.
   */
  MeshHandle add(const MeshData& mesh, std::string name = {});

  /** @return the mesh's name, or empty if it has none or isn't ours. */
  std::string_view nameOf(MeshHandle mesh) const;

  /** @return the mesh with this name, or an invalid handle. */
  MeshHandle find(std::string_view name) const;

  /**
   * A stand-in for a named mesh that isn't available (e.g. one a game module
   * provides, before the module is built). It keeps the name, so a scene saved
   * meanwhile still refers to the mesh, but has no geometry and draws nothing.
   * The same name always gives the same handle.
   */
  MeshHandle missing(std::string_view name);

  /** Whether `mesh` is a stand-in from missing(). */
  bool isMissing(MeshHandle mesh) const;

  /** Every named mesh (not stand-ins), sorted by name (e.g. for a mesh picker). */
  std::vector<std::pair<MeshHandle, std::string>> named() const;

  /** Free a mesh this library added. Other handles are ignored. */
  void remove(MeshHandle mesh);

  /** @return the mesh's local-space bounds, or nullptr if it isn't ours (or is missing). */
  const Aabb* bounds(MeshHandle mesh) const;

  std::size_t size() const { return m_meshes.size(); }

private:
  struct Entry
  {
    Aabb bounds;
    std::string name;
    bool missing = false; // a stand-in: no renderer mesh behind it
  };

  // Stand-in ids, from the top of the range so they never meet the renderer's.
  static constexpr uint32_t kMissingIdBase = 0x80000000u;

  IRenderer3D& m_renderer;
  std::unordered_map<uint32_t, Entry> m_meshes;
  uint32_t m_nextMissingId = kMissingIdBase;
};

} // namespace sfs
