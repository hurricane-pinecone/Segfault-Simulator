#pragma once

#include "engine/core/rendering/3d/aabb.h"
#include "engine/core/rendering/3d/camera3D.h"
#include "engine/core/rendering/3d/mesh.h"
#include "glm/glm/ext/matrix_float4x4.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
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
   * Somewhere more meshes come from on demand, by name (e.g. model assets):
   * the library asks it for a name it doesn't have.
   */
  struct Provider
  {
    // Add the named mesh to `library` (with add) and return it, or return an
    // invalid handle if the name isn't one it provides or can't be loaded.
    std::function<MeshHandle(MeshLibrary& library, std::string_view name)> load;
    // Names it could load, each with a label for people, e.g. for a picker.
    std::function<std::vector<std::pair<std::string, std::string>>()> available;
    // A label for one of its names ("models/barn.glb"); empty if it isn't one.
    std::function<std::string(std::string_view name)> label;
    // A named material (e.g. a material asset), or null.
    std::function<std::shared_ptr<const MeshMaterial>(std::string_view name)> loadMaterial;
    // Material and texture names it has, each with a label, e.g. for a picker.
    std::function<std::vector<std::pair<std::string, std::string>>()> materials;
    std::function<std::vector<std::pair<std::string, std::string>>()> textures;
  };

  /** Set (or with {} clear) where meshes this library lacks come from. */
  void setProvider(Provider provider) { m_provider = std::move(provider); }

  /**
   * The mesh with this name, loading it from the provider if the library has
   * nothing by that name yet. An invalid handle if neither has it.
   */
  MeshHandle resolve(std::string_view name);

  /** The provider's meshes that aren't loaded yet, as (name, label), sorted by label. */
  std::vector<std::pair<std::string, std::string>> available() const;

  /** How to show a mesh name to people: the provider's label, else the name itself. */
  std::string label(std::string_view name) const;

  /**
   * A mesh's sections (its material slots) with their own materials: at least
   * one, the default material, for a mesh without sections. Empty if the mesh
   * isn't ours.
   */
  std::vector<MeshSection> sections(MeshHandle mesh) const;

  struct RayHit
  {
    float distance = 0.0f;   // along the ray, in its direction's units
    std::size_t section = 0; // which section (material slot) the triangle is in
  };

  /**
   * The nearest of a mesh's triangles (either side) a ray hits, with the mesh
   * placed by `world`. Nothing if it misses, or the mesh isn't ours.
   */
  std::optional<RayHit> raycast(MeshHandle mesh, const glm::mat4& world, const Ray3D& ray) const;

  /**
   * A material by name (an entry of MeshRendererComponent::materials),
   * uploaded through the provider the first time it's asked for and kept.
   * Invalid if the name is empty or the provider has no such material (asked
   * only once).
   */
  MaterialHandle material(std::string_view name);

  /**
   * Ask the provider for a material again (e.g. its file was edited) and
   * update it in place, if it's loaded.
   */
  void reloadMaterial(std::string_view name);

  /** The provider's materials and textures, as (name, label), sorted by label. */
  std::vector<std::pair<std::string, std::string>> availableMaterials() const;
  std::vector<std::pair<std::string, std::string>> availableTextures() const;

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
    std::vector<MeshSection> sections; // the material slots
    // The triangles, kept for raycasts (e.g. picking where something drops).
    std::vector<glm::vec3> positions;
    std::vector<uint32_t> indices;
  };

  // Stand-in ids, from the top of the range so they never meet the renderer's.
  static constexpr uint32_t kMissingIdBase = 0x80000000u;

  IRenderer3D& m_renderer;
  Provider m_provider;
  std::unordered_map<uint32_t, Entry> m_meshes;
  uint32_t m_nextMissingId = kMissingIdBase;
  std::unordered_map<std::string, MaterialHandle> m_materials; // invalid: not found
};

} // namespace sfs
