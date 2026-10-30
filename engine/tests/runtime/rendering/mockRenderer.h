#pragma once

#include <engine/runtime/rendering/iRenderer.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

namespace sfs::testing3d
{

// Records what the render system hands the renderer; no GPU is touched.
class MockRenderer : public sfs::IRenderer
{
public:
  MeshHandle createMesh(const MeshData& mesh) override
  {
    if (mesh.empty())
      return {};
    live.push_back(++nextId);
    return MeshHandle{nextId};
  }

  void destroyMesh(MeshHandle mesh) override
  {
    live.erase(std::remove(live.begin(), live.end(), mesh.id), live.end());
  }

  MaterialHandle createMaterial(const MeshMaterial&) override
  {
    liveMaterials.push_back(++nextMaterialId);
    return MaterialHandle{nextMaterialId};
  }

  void updateMaterial(MaterialHandle, const MeshMaterial&) override
  {
    ++materialUpdates;
  }

  void destroyMaterial(MaterialHandle material) override
  {
    liveMaterials.erase(
        std::remove(liveMaterials.begin(), liveMaterials.end(), material.id),
        liveMaterials.end());
  }

  GrassHandle createGrass(const GrassGround& ground) override
  {
    if (ground.empty())
      return {};
    const GrassHandle handle{++nextGrassId};
    liveGrass[handle.id] = ground.groundTexels();
    return handle;
  }

  void updateGrassMaps(GrassHandle grass, const GrassMapsImage& image) override
  {
    if (liveGrass.contains(grass.id))
      mapUpdates.push_back({grass, image.grid, image.from, image.to});
  }

  void updateGrassTrample(GrassHandle grass,
                          const GrassTrampleImage& image) override
  {
    if (liveGrass.contains(grass.id))
      trampleUpdates.push_back({grass, image.size});
  }

  void destroyGrass(GrassHandle grass) override { liveGrass.erase(grass.id); }

  void drawGrass(const GrassBatch& batch) override
  {
    grassDrawn.push_back(batch);
  }

  void drawUi(const UiDrawList& list) override { uiDrawn = list.quads; }

  glm::ivec2 viewportSize() const override { return viewport; }
  uint32_t shadowMapResolution() const override { return 1024; }

  void beginFrame(const FrameParams& params) override
  {
    frame = params;
    lights.assign(params.pointLights.begin(), params.pointLights.end());
    batches.clear();
    drawn.clear();
    grassDrawn.clear();
    uiDrawn.clear();
    ++frames;
  }

  void drawMesh(const MeshBatch& batch) override
  {
    batches.push_back({batch.mesh,
                       batch.instances.size(),
                       batch.shadowInstances.size(),
                       {batch.materials.begin(), batch.materials.end()},
                       batch.lightShadowInstances.size(),
                       batch.together});
    drawn.insert(drawn.end(), batch.instances.begin(), batch.instances.end());
  }

  void endFrame() override {}

  struct Batch
  {
    MeshHandle mesh;
    std::size_t instances = 0;
    std::size_t shadowInstances = 0;
    std::vector<MaterialHandle> materials;
    std::size_t lightShadowInstances = 0;
    bool together = false;
  };

  glm::ivec2 viewport{1280, 720};
  uint32_t nextId = 0;
  std::vector<uint32_t> live;
  uint32_t nextMaterialId = 0;
  std::vector<uint32_t> liveMaterials;
  int materialUpdates = 0;
  FrameParams frame;
  std::vector<PointLight> lights;
  std::vector<Batch> batches;
  std::vector<MeshInstance> drawn; // every lit instance this frame
  int frames = 0;

  uint32_t nextGrassId = 0;
  std::map<uint32_t, std::size_t> liveGrass; // ground texels, by handle
  std::vector<GrassBatch> grassDrawn;        // this frame
  std::vector<UiQuad> uiDrawn;               // this frame's UI
  struct MapUpdate
  {
    GrassHandle grass;
    GrassGrid grid;
    glm::ivec2 from, to;
  };
  std::vector<MapUpdate> mapUpdates; // all of them
  struct TrampleUpdate
  {
    GrassHandle grass;
    glm::ivec2 size;
  };
  std::vector<TrampleUpdate> trampleUpdates; // all of them
};

} // namespace sfs::testing3d
