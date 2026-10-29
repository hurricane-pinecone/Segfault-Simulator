#pragma once

#include "engine/core/assets/assetDatabase.h"
#include "engine/webgpu/wgpuHandle.h"

#include "imgui.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace sfs
{
class AssetStore;
class WebGpuContext;
class WorkerPool;
} // namespace sfs

class EditorScene;

// Preview images of assets, for the Assets panel: textures scaled down,
// materials on a sphere, models and prefabs drawn from a three-quarter view
// (see sfs::meshPreview). Each is drawn on the CPU on a worker thread,
// uploaded as a small texture and kept, and drawn again when its file
// changes.
class Thumbnails
{
public:
  static constexpr int kSize = 128; // pixels square; drawn smaller as needed

  explicit Thumbnails(sfs::WebGpuContext& context);
  ~Thumbnails();

  /** Call once a frame, before get(): takes in the previews finished since. */
  void beginFrame();

  /**
   * The asset's preview, or 0 if it has none (not a texture, material, model
   * or prefab, or it can't be read) or it isn't made yet (it will be soon).
   * Prefabs are drawn with `scene`'s meshes (the game module's included).
   */
  ImTextureID get(const sfs::AssetRecord& record,
                  const sfs::AssetDatabase& assets,
                  sfs::AssetStore& store,
                  EditorScene* scene);

  /** Forget every preview (e.g. another project was opened). */
  void clear() { m_entries.clear(); }

  /** Check shown previews' files again (they may have changed). */
  void recheck() { ++m_generation; }

  /** Previews made since none were being made: how many are done, of how many. */
  struct Progress
  {
    int done = 0;
    int total = 0;
  };
  Progress progress() const { return m_progress; }

private:
  struct Entry
  {
    sfs::wgpu::Texture texture;
    sfs::wgpu::TextureView view; // null: nothing to show
    std::filesystem::file_time_type stamp;
    uint64_t checked = 0; // the generation its file was last checked in
    uint64_t ticket = 0;  // the preview being made for it (0: none)
  };

  struct Finished
  {
    sfs::AssetId id;
    uint64_t ticket = 0;
    std::vector<uint8_t> rgba;
  };

  using Job = std::function<std::vector<uint8_t>()>;

  // The work for an asset's preview, gathered now (on the main thread) to run
  // on a worker; nothing to do yet if a prefab's meshes are still loading.
  std::optional<Job> makeJob(const sfs::AssetRecord& record,
                             const sfs::AssetDatabase& assets,
                             sfs::AssetStore& store,
                             EditorScene* scene);
  bool upload(Entry& entry, const std::vector<uint8_t>& rgba);

  sfs::WebGpuContext& m_context;
  std::unordered_map<sfs::AssetId, Entry, sfs::AssetIdHash> m_entries;
  int m_budget = 0; // previews still to start this frame
  // Files are checked once per generation, not continually: on a network
  // share (WSL) each check takes about a millisecond.
  uint64_t m_generation = 1;
  uint64_t m_nextTicket = 1;
  Progress m_progress;
  std::mutex m_finishedMutex;
  std::vector<Finished> m_finished; // filled by the workers
  std::unique_ptr<sfs::WorkerPool> m_workers; // last: stopped before the rest goes
};
