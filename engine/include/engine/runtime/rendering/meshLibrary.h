#pragma once

#include "engine/core/rendering/aabb.h"
#include "engine/core/rendering/camera.h"
#include "engine/core/rendering/emissionLights.h"
#include "engine/core/rendering/mesh.h"
#include "glm/glm/ext/matrix_float4x4.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sfs
{

class IRenderer;

/**
 * The meshes a scene has uploaded: wraps IRenderer's create/destroy and keeps
 * each mesh's local bounds for culling. Owns its meshes, so they are freed with
 * the library (i.e. with the scene's render system).
 */
class MeshLibrary
{
public:
  explicit MeshLibrary(IRenderer& renderer);
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

  /**
   * Keep every mesh's vertices on the CPU too (all libraries, from meshes
   * added after the call), for meshData(). Off by default (it costs memory);
   * an editor turns it on to draw previews.
   */
  static void setKeepVertices(bool keep);

  /** A mesh's data as added (vertices kept only with setKeepVertices), or
   * nothing. */
  std::optional<MeshData> meshData(MeshHandle mesh) const;

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
    std::function<std::shared_ptr<const MeshMaterial>(std::string_view name)>
        loadMaterial;
    // Material and texture names it has, each with a label, e.g. for a picker.
    std::function<std::vector<std::pair<std::string, std::string>>()> materials;
    std::function<std::vector<std::pair<std::string, std::string>>()> textures;
    // For meshes loaded in the background: load() gives a stand-in from
    // pending() while one loads, and this gives its data once it has
    // (null while `waiting`, or if it failed).
    std::function<const MeshData*(std::string_view name, bool& waiting)> loaded;
  };

  /** Set (or with {} clear) where meshes this library lacks come from. */
  void setProvider(Provider provider) { m_provider = std::move(provider); }

  /**
   * The mesh with this name, loading it from the provider if the library has
   * nothing by that name yet. An invalid handle if neither has it.
   */
  MeshHandle resolve(std::string_view name);

  /** The provider's meshes that aren't loaded yet, as (name, label), sorted by
   * label. */
  std::vector<std::pair<std::string, std::string>> available() const;

  /** How to show a mesh name to people: the provider's label, else the name
   * itself. */
  std::string label(std::string_view name) const;

  /**
   * A mesh's sections (its material slots) with their own materials: at least
   * one, the default material, for a mesh without sections. Empty if the mesh
   * isn't ours.
   */
  std::vector<MeshSection> sections(MeshHandle mesh) const;

  /** How many sections sections() gives, without copying them. */
  std::size_t sectionCount(MeshHandle mesh) const;

  /**
   * A mesh's triangles, as added: positions (its own space) and the indices
   * into them, three a triangle. Empty if the mesh isn't ours (or is a
   * stand-in).
   */
  std::span<const glm::vec3> positions(MeshHandle mesh) const;
  std::span<const uint32_t> indices(MeshHandle mesh) const;

  /** Section `section`'s own material (as sections() has it), or null. */
  const MeshMaterial* sectionMaterial(MeshHandle mesh,
                                      std::size_t section) const;

  /**
   * Points spread evenly over a section's surface (see sampleSurface()),
   * taken when the mesh is added: where emission lighting reads how much a
   * glowing material gives off. Empty if the mesh or section isn't ours.
   */
  std::span<const SurfaceSample> surfaceSamples(MeshHandle mesh,
                                                std::size_t section) const;

  struct RayHit
  {
    float distance = 0.0f;   // along the ray, in its direction's units
    std::size_t section = 0; // which section (material slot) the triangle is in
  };

  /**
   * The nearest of a mesh's triangles (either side) a ray hits, with the mesh
   * placed by `world`. Nothing if it misses, or the mesh isn't ours.
   */
  std::optional<RayHit>
  raycast(MeshHandle mesh, const glm::mat4& world, const Ray& ray) const;

  /**
   * A material by name (an entry of MeshRendererComponent::materials),
   * uploaded through the provider the first time it's asked for and kept.
   * Invalid if the name is empty or the provider has no such material (asked
   * only once).
   */
  MaterialHandle material(std::string_view name);

  /**
   * Make (or change) a named material from game code, e.g. one whose texture
   * the game paints as it runs. Entities choose it by name in a material slot;
   * changing it shows on everything using it.
   */
  MaterialHandle setMaterial(std::string_view name,
                             const MeshMaterial& material);

  /**
   * Ask the provider for a material again (e.g. its file was edited) and
   * update it in place, if it's loaded.
   */
  void reloadMaterial(std::string_view name);

  /**
   * What a material made by material() or setMaterial() is, e.g. to work out
   * its emission on the CPU. Null for any other handle.
   */
  const MeshMaterial* materialData(MaterialHandle material) const;

  /** The provider's materials and textures, as (name, label), sorted by label.
   */
  std::vector<std::pair<std::string, std::string>> availableMaterials() const;
  std::vector<std::pair<std::string, std::string>> availableTextures() const;

  /**
   * A stand-in for a named mesh that isn't available (e.g. one a game module
   * provides, before the module is built). It keeps the name, so a scene saved
   * meanwhile still refers to the mesh, but has no geometry and draws nothing.
   * The same name always gives the same handle.
   */
  MeshHandle missing(std::string_view name);

  /** Whether `mesh` is a stand-in from missing() (or pending(), still loading).
   */
  bool isMissing(MeshHandle mesh) const;

  /**
   * Whether named meshes it lacks are expected to come later: a scene loaded
   * before its game module (which provides them) is built. Loading then keeps
   * their stand-ins (missing()) without reporting each as an error.
   */
  void setMissingExpected(bool expected) { m_missingExpected = expected; }
  bool missingExpected() const { return m_missingExpected; }

  /**
   * A stand-in for a named mesh being loaded in the background (see
   * Provider::loaded): like missing(), but fulfilPending() turns it into the
   * mesh, under the same handle, once it's loaded.
   */
  MeshHandle pending(std::string_view name);
  bool isPending(MeshHandle mesh) const;
  std::size_t pendingCount() const;

  /** Turn stand-ins whose meshes have loaded into them (StandardScene calls
   * this). */
  void fulfilPending();

  /**
   * The renderer's handle for a mesh: the same, except for a stand-in that
   * has since loaded. Draw commands carry this one.
   */
  MeshHandle gpuHandle(MeshHandle mesh) const;

  /** Every named mesh (not stand-ins), sorted by name (e.g. for a mesh picker).
   */
  std::vector<std::pair<MeshHandle, std::string>> named() const;

  /** Free a mesh this library added. Other handles are ignored. */
  void remove(MeshHandle mesh);

  /** @return the mesh's local-space bounds, or nullptr if it isn't ours (or is
   * missing). */
  const Aabb* bounds(MeshHandle mesh) const;

  std::size_t size() const { return m_meshes.size(); }

private:
  struct Entry
  {
    Aabb bounds;
    std::string name;
    bool missing = false;              // a stand-in: no renderer mesh behind it
    std::vector<MeshSection> sections; // the material slots
    // The triangles, kept for raycasts (e.g. picking where something drops).
    std::vector<glm::vec3> positions;
    std::vector<uint32_t> indices;
    std::vector<MeshVertex> vertices; // only with setKeepVertices
    std::vector<std::vector<SurfaceSample>> samples; // per section
    MeshHandle gpu;       // the renderer's mesh (none while missing)
    bool pending = false; // a stand-in loading in the background
  };

  // Fill an entry from mesh data, uploading it.
  bool fill(Entry& entry, const MeshData& mesh);

  // Stand-in ids, from the top of the range so they never meet the renderer's.
  static constexpr uint32_t kMissingIdBase = 0x80000000u;

  IRenderer& m_renderer;
  Provider m_provider;
  std::unordered_map<uint32_t, Entry> m_meshes;
  uint32_t m_nextMissingId = kMissingIdBase;
  bool m_missingExpected = false;
  // Looked up by string_view without making a string (MeshModule asks for
  // every drawn entity's materials every frame).
  struct NameHash
  {
    using is_transparent = void;
    std::size_t operator()(std::string_view name) const
    {
      return std::hash<std::string_view>()(name);
    }
  };
  std::unordered_map<std::string, MaterialHandle, NameHash, std::equal_to<>>
      m_materials; // invalid: not found
  // What each of m_materials is, by handle id.
  std::unordered_map<uint32_t, std::shared_ptr<const MeshMaterial>>
      m_materialData;
};

} // namespace sfs
