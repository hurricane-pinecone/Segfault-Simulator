#pragma once

#include <engine/runtime/rendering3d/iRenderer3D.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace sfs::testing3d
{

// Records what the render system hands the renderer; no GPU is touched.
class MockRenderer3D : public sfs::IRenderer3D
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

  glm::ivec2 viewportSize() const override { return viewport; }
  uint32_t shadowMapResolution() const override { return 1024; }

  void beginFrame(const FrameParams3D& params) override
  {
    frame = params;
    lights.assign(params.pointLights.begin(), params.pointLights.end());
    batches.clear();
    drawn.clear();
    ++frames;
  }

  void drawMesh(const MeshBatch3D& batch) override
  {
    batches.push_back({batch.mesh,
                       batch.instances.size(),
                       batch.shadowInstances.size(),
                       {batch.materials.begin(), batch.materials.end()}});
    drawn.insert(drawn.end(), batch.instances.begin(), batch.instances.end());
  }

  void endFrame() override {}

  struct Batch
  {
    MeshHandle mesh;
    std::size_t instances = 0;
    std::size_t shadowInstances = 0;
    std::vector<MaterialHandle> materials;
  };

  glm::ivec2 viewport{1280, 720};
  uint32_t nextId = 0;
  std::vector<uint32_t> live;
  uint32_t nextMaterialId = 0;
  std::vector<uint32_t> liveMaterials;
  int materialUpdates = 0;
  FrameParams3D frame;
  std::vector<PointLight3D> lights;
  std::vector<Batch> batches;
  std::vector<MeshInstance> drawn; // every lit instance this frame
  int frames = 0;
};

} // namespace sfs::testing3d
