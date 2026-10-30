#pragma once

#include "engine/core/assets/assetDatabase.h"
#include "engine/runtime/assetStore/modelData.h"

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float4.hpp"

#include <SDL_surface.h>
#include <SDL_ttf.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sfs
{

class WorkerPool;
struct MaterialAsset;

using SurfacePtr = std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)>;
using FontPtr = std::unique_ptr<TTF_Font, decltype(&TTF_CloseFont)>;

/**
 * The loaded assets a game draws with: images (as CPU surfaces), fonts and
 * models. Renderers keep their own GPU
 * copies, keyed the same way.
 *
 * Images and fonts are added from file paths under names the game
 * picks. Given the project's AssetDatabase, the store also loads assets by
 * their AssetId (models, materials and textures), keeping each until
 * cleared.
 */
class AssetStore
{
public:
  AssetStore();
  ~AssetStore();

  void clearAssets();

  /**
   * The catalogue to load assets by id from (the project's assets/); null for
   * none. Changing it forgets the models loaded from the previous one.
   */
  void setDatabase(const AssetDatabase* database);
  const AssetDatabase* database() const { return m_database; }

  /**
   * A model asset's meshes, imported (with its .meta import settings) on first
   * use and kept. Null (logged once) if the id isn't a model in the database
   * or its file can't be imported.
   */
  const ModelData* model(const AssetId& id);

  /** The model if it's already loaded, without loading it. */
  const ModelData* loadedModel(const AssetId& id) const;

  /** Forget a loaded model, so the next model() reads its file again. */
  void forgetModel(const AssetId& id);

  // --- loading in the background ---------------------------------------------

  /**
   * Import models on worker threads when they're asked for with
   * requestModel(), rather than there and then, so a scene opens at once and
   * its models appear as they load (an editor). Off by default: a game's
   * scene is complete when it starts.
   */
  void setBackgroundLoading(bool background);
  bool backgroundLoading() const { return m_background; }

  enum class LoadState
  {
    Loaded,  // *model is set
    Loading, // in the background; ask again later
    Failed,  // not a model, or its file can't be imported (logged)
  };

  /**
   * A model, if it's loaded; otherwise start loading it, in the background
   * when that's on (else now, as model() does).
   */
  LoadState requestModel(const AssetId& id, const ModelData** model);

  /**
   * Take in the models finished in the background since the last call. Call
   * once a frame (each StandardScene does, as it updates).
   */
  void collectLoads();

  /** Bumped whenever background loads are taken in (see collectLoads()). */
  uint64_t loadGeneration() const { return m_loadGeneration; }

  /** Background loads since nothing was loading: how many have finished, of how
   * many. */
  struct LoadProgress
  {
    int done = 0;
    int total = 0;
  };
  LoadProgress loadProgress() const { return m_progress; }

  /**
   * A texture asset's texels, read (with its .meta import settings)
   * on first use and kept. Null (logged once) if the id isn't a texture in the
   * database or its file can't be read.
   */
  std::shared_ptr<const TextureData> texture(const AssetId& id);

  /**
   * The same texels read as data (metalness, roughness, masks), not colour:
   * texture(), marked linear. Kept too, so every material reading it as data
   * shares one copy (and one upload).
   */
  std::shared_ptr<const TextureData> dataTexture(const AssetId& id);

  /** Read a texture asset again next time (its file or settings changed). */
  void forgetTexture(const AssetId& id);

  /**
   * A material asset (.mat), with its textures loaded, read on first use and
   * kept. Null (logged once) if the id isn't a material in the database or
   * its file can't be read.
   */
  std::shared_ptr<const MeshMaterial> material(const AssetId& id);

  /**
   * A material as a .mat file holds it (e.g. one being edited, not saved
   * yet), with its textures loaded and, for a node-graph material, its graph
   * compiled. A graph that doesn't compile draws magenta; its errors are
   * logged, naming `source`.
   */
  std::shared_ptr<MeshMaterial> makeMaterial(const MaterialAsset& asset,
                                             const std::string& source);

  /** Forget a loaded material, so the next material() reads its file again. */
  void forgetMaterial(const AssetId& id);

  /**
   * A prefab asset's contents (a .prefab file; see SceneSerializer), read on
   * first use and kept. A model is a prefab too: its node tree, each node an
   * entity drawing its part (or, with one mesh, a single entity). Nothing
   * (logged once) if the id isn't either in the database or can't be read.
   */
  std::optional<nlohmann::json> prefab(const AssetId& id);

  /** Forget a loaded prefab, so the next prefab() reads its file again. */
  void forgetPrefab(const AssetId& id);

  // --- letting go of what's no longer used
  // ------------------------------------

  /**
   * Start noting which assets loaded by id (models, textures, materials,
   * prefabs) are asked for, e.g. as a new scene loads; collectUnused() then
   * lets go of the rest. A game does this as it changes scenes.
   */
  void beginUseTracking();

  /**
   * Forget the assets loaded by id that haven't been asked for since
   * beginUseTracking() (keeping textures a kept material uses). What still
   * holds one (a renderer's copy) keeps it alive; asked for again, it's read
   * again.
   *
   * @return how many were let go
   */
  std::size_t collectUnused();

  void addTexture(const std::string& assetId, const std::string& filePath);
  void removeTexture(const std::string& assetId);
  SDL_Surface* getSurface(const std::string& assetId) const;

  /**
   * An image as texels the renderer draws from: one added by name (a sprite
   * sheet, sampled without smoothing: pixel art), converted on first use and
   * kept, or a texture asset ("asset:<id>", see texture()). Null if there's no
   * such image.
   */
  std::shared_ptr<const TextureData> imageTexture(const std::string& assetId);

  /** One frame of a sprite sheet: its image, and where the frame is on it. */
  struct SpriteFrame
  {
    std::shared_ptr<const TextureData> sheet;
    glm::vec4 uvRect{0.0f, 0.0f, 1.0f, 1.0f}; // x, y, width, height in 0..1
    glm::vec2 pixels{0.0f};                   // the frame's size
    float pixelsPerUnit = 0.0f; // its import setting; 0: the scene's
  };

  /**
   * Frame `frame` of an image (as imageTexture() names it): of a texture
   * asset's sprite grid (its import settings' spriteCell; counted across then
   * down, wrapping past the last), or the whole image if it has none, with
   * its import settings' pixelsPerUnit. Empty if there's no such image.
   */
  std::optional<SpriteFrame> spriteFrame(const std::string& image, int frame);

  TTF_Font*
  addFont(const std::string& fontId, const std::string& filePath, int size);

  void removeFont(const std::string& fontId);
  TTF_Font* getFont(const std::string& fontId) const;

  AssetStore(const AssetStore&) = delete;
  AssetStore& operator=(const AssetStore&) = delete;

  void addWhitePixelTexture(const std::string& textureId);

  // Generates a soft round texture: white RGB with a radial alpha falloff (1 at
  // the centre, 0 at the edge). Handy for particles/decals so they don't read
  // as hard squares. `size` is the texture's pixel dimension.
  void addRadialTexture(const std::string& textureId, int size);

private:
  std::unordered_map<std::string, SurfacePtr> m_surfaces;
  std::unordered_map<std::string, FontPtr> m_fonts;

  // A model's node tree as prefab content (see prefab()).
  static nlohmann::json modelPrefab(const std::vector<ModelNode>& nodes,
                                    const AssetId& id,
                                    const std::string& name);

  const AssetDatabase* m_database = nullptr;

  // Background loading. A load's ticket must still be its model's when it
  // finishes, or it's dropped (the model was forgotten, or loaded at once,
  // meanwhile).
  struct FinishedLoad
  {
    AssetId id;
    uint64_t ticket = 0;
    std::optional<ModelData> model;
  };
  bool m_background = false;
  std::unique_ptr<WorkerPool> m_workers;
  std::unordered_map<AssetId, uint64_t, AssetIdHash> m_loading; // id -> ticket
  uint64_t m_nextTicket = 1;
  std::mutex m_finishedMutex;
  std::vector<FinishedLoad> m_finished; // filled by the workers
  uint64_t m_loadGeneration = 0;
  LoadProgress m_progress;
  // nullopt: tried and failed (so it isn't retried, or logged, every time).
  std::unordered_map<AssetId, std::optional<ModelData>, AssetIdHash> m_models;
  // Null: tried and failed.
  std::unordered_map<AssetId, std::shared_ptr<const TextureData>, AssetIdHash>
      m_textures;
  // Images added by name, as texels (imageTexture()).
  std::unordered_map<std::string, std::shared_ptr<const TextureData>>
      m_imageTextures;
  // Textures read as data (dataTexture()).
  std::unordered_map<AssetId, std::shared_ptr<const TextureData>, AssetIdHash>
      m_dataTextures;
  std::unordered_map<AssetId, std::shared_ptr<const MeshMaterial>, AssetIdHash>
      m_materials;
  std::unordered_map<AssetId, std::optional<nlohmann::json>, AssetIdHash>
      m_prefabs; // nullopt: failed

  // When each asset loaded by id was last asked for (see beginUseTracking()).
  void used(const AssetId& id) { m_lastUse[id] = m_useEpoch; }
  std::unordered_map<AssetId, uint64_t, AssetIdHash> m_lastUse;
  uint64_t m_useEpoch = 0;
};

} // namespace sfs
