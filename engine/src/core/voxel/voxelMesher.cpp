#include "engine/core/voxel/voxelMesher.h"

#include "engine/core/voxel/voxelChunk.h"
#include "glm/glm/common.hpp"

#include <string>
#include <unordered_map>

namespace sfs
{

namespace
{

// Elevation's weight in the painter sort-key, matching BlockGeometry and the
// billboard tile sort so voxels interleave with sprites the same way.
constexpr float kElevationSortWeight = 0.5f;

// Fractional coordinate within a block's sprite. The cube sprite (block.png)
// packs a 2:1 top diamond over the +y (screen-left) and +x (screen-right) side
// faces -- the same layout BlockGeometry uses, so voxels read identically.
struct UvF
{
  float x;
  float y;
};

constexpr UvF kDiamondTop{0.5f, 0.0f};
constexpr UvF kDiamondRight{1.0f, 0.25f};
constexpr UvF kDiamondBottom{0.5f, 0.5f};
constexpr UvF kDiamondLeft{0.0f, 0.25f};

constexpr UvF kLeftTopOuter{0.0f, 0.25f};
constexpr UvF kLeftTopInner{0.5f, 0.5f};
constexpr UvF kLeftBotInner{0.5f, 1.0f};
constexpr UvF kLeftBotOuter{0.0f, 0.75f};

constexpr UvF kRightTopInner{0.5f, 0.5f};
constexpr UvF kRightTopOuter{1.0f, 0.25f};
constexpr UvF kRightBotOuter{1.0f, 0.75f};
constexpr UvF kRightBotInner{0.5f, 1.0f};

// The diamond + the two side faces, in the order pushQuad expects.
struct FaceUv
{
  UvF diamond[4];
  UvF left[4];
  UvF right[4];
};

// Full cube: the whole cube sprite (block.png layout).
constexpr FaceUv kCubeUv{
    {kDiamondTop, kDiamondRight, kDiamondBottom, kDiamondLeft},
    {kLeftTopOuter, kLeftTopInner, kLeftBotInner, kLeftBotOuter},
    {kRightTopInner, kRightTopOuter, kRightBotOuter, kRightBotInner}};

// Slab (half block): same diamond, but each side is the TOP HALF of the cube's
// two-level side (a one-level-tall side). A slab is the cube minus its lower
// level, so it reuses the cube sprite -- no separate half-block art needed.
constexpr FaceUv kSlabUv{
    {kDiamondTop, kDiamondRight, kDiamondBottom, kDiamondLeft},
    {{0.0f, 0.25f}, {0.5f, 0.5f}, {0.5f, 0.75f}, {0.0f, 0.5f}},
    {{0.5f, 0.5f}, {1.0f, 0.25f}, {1.0f, 0.5f}, {0.5f, 0.75f}}};

// Map a sprite fraction into the block type's normalised uv sub-rect.
glm::vec2 uvIn(const glm::vec4& rect, UvF f)
{
  return {rect.x + f.x * (rect.z - rect.x), rect.y + f.y * (rect.w - rect.y)};
}

void pushQuad(std::vector<GeometryVertex>& out,
              const glm::vec2 world[4],
              const float ground[4],
              const glm::vec2 uv[4],
              const glm::vec3& normal)
{
  GeometryVertex v[4];
  for (int i = 0; i < 4; i++)
  {
    v[i].position = {0.0f, 0.0f}; // filled by the render module at emit
    v[i].worldPos = world[i];
    v[i].ground = ground[i];
    v[i].uv = uv[i];
    v[i].normal = normal;
    v[i].z = world[i].x + world[i].y + ground[i] * kElevationSortWeight;
  }
  out.push_back(v[0]);
  out.push_back(v[1]);
  out.push_back(v[2]);
  out.push_back(v[0]);
  out.push_back(v[2]);
  out.push_back(v[3]);
}

} // namespace

std::vector<VoxelMeshSlice> meshChunk(glm::ivec3 chunkCoord,
                                      const IVoxelView& view,
                                      const IBlockRegistry& registry)
{
  std::unordered_map<std::string, VoxelMeshSlice> buckets;
  const glm::ivec3 base = chunkCoord * kChunkSize;

  // A neighbour hides a face only if it is a solid, opaque block (used for the
  // horizontal top face, which a same-footprint neighbour either fully covers
  // or not).
  const auto opaque = [&](int x, int y, int z)
  {
    const BlockId n = view.blockAt(x, y, z);
    return n != kAirBlock && registry.type(n).opaque;
  };

  // For a vertical side, the elevation up to which the same-cell neighbour
  // hides it: a cube covers the whole cell, a slab only its lower level,
  // air/liquid nothing. The face above this is still visible -- so a slab next
  // to a taller block leaves the upper part of that block's face showing (no
  // hole).
  const auto coverTop = [&](int x, int y, int z) -> float
  {
    const BlockId n = view.blockAt(x, y, z);
    if (n == kAirBlock)
      return static_cast<float>(z * kLevelsPerBlock);
    const BlockType& nt = registry.type(n);
    if (!nt.opaque)
      return static_cast<float>(z * kLevelsPerBlock);
    return nt.shape == BlockShape::Slab
               ? static_cast<float>(z * kLevelsPerBlock + 1)
               : static_cast<float>((z + 1) * kLevelsPerBlock);
  };

  const auto lerpUv = [](UvF a, UvF b, float t) {
    return UvF{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
  };

  for (int lz = 0; lz < kChunkSize; ++lz)
    for (int ly = 0; ly < kChunkSize; ++ly)
      for (int lx = 0; lx < kChunkSize; ++lx)
      {
        const int wx = base.x + lx;
        const int wy = base.y + ly;
        const int wz = base.z + lz;

        const BlockId id = view.blockAt(wx, wy, wz);
        if (id == kAirBlock)
          continue;

        const BlockType& bt = registry.type(id);

        // Liquids are physical but emit no opaque geometry -- the water render
        // path draws their surface. (Being non-opaque, the solid terrain under
        // and behind them still meshes its now-exposed faces.)
        if (bt.liquid)
          continue;

        VoxelMeshSlice* slice = nullptr;
        const auto ensure = [&]() -> VoxelMeshSlice&
        {
          if (!slice)
          {
            const std::string key = *bt.textureId + '|' +
                                    std::to_string(static_cast<int>(bt.effect));
            slice = &buckets[key];
            slice->textureId = bt.textureId;
            slice->effect = bt.effect;
          }
          return *slice;
        };

        const float fx = static_cast<float>(wx);
        const float fy = static_cast<float>(wy);
        const bool slab = bt.shape == BlockShape::Slab;
        const float botElev = static_cast<float>(wz * kLevelsPerBlock);
        // A slab fills the lower level of its cell; a cube fills both.
        const float topElev =
            slab ? botElev + 1.0f
                 : static_cast<float>((wz + 1) * kLevelsPerBlock);
        const FaceUv& uvs = slab ? kSlabUv : kCubeUv;

        // +z (top diamond). A slab's top is always exposed -- the upper level
        // of its own cell is air -- so it doesn't test the cell above; a cube's
        // top is hidden when the cell above is opaque.
        if (slab || !opaque(wx, wy, wz + 1))
        {
          const glm::vec2 world[4] = {{fx, fy},
                                      {fx + 1.0f, fy},
                                      {fx + 1.0f, fy + 1.0f},
                                      {fx, fy + 1.0f}};
          const float g[4] = {topElev, topElev, topElev, topElev};
          const glm::vec2 uv[4] = {uvIn(bt.uvRect, uvs.diamond[0]),
                                   uvIn(bt.uvRect, uvs.diamond[1]),
                                   uvIn(bt.uvRect, uvs.diamond[2]),
                                   uvIn(bt.uvRect, uvs.diamond[3])};
          pushQuad(ensure().vertices, world, g, uv, {0.0f, 0.0f, 1.0f});
        }

        // A side face, clipped to the part its same-cell neighbour doesn't
        // cover. world corners are {outerTop, innerTop, innerBot, outerBot};
        // the two bottoms ride up to faceBot with their uv interpolated, so a
        // partially-covered face keeps the correct texture slice.
        const auto emitSide = [&](int nx,
                                  int ny,
                                  glm::vec2 outer,
                                  glm::vec2 inner,
                                  const UvF face[4],
                                  const glm::vec3& normal)
        {
          const float cover = coverTop(nx, ny, wz);
          const float faceBot = glm::max(botElev, cover);
          if (topElev - faceBot <= 1.0e-4f)
            return;

          const float t = (topElev - faceBot) / (topElev - botElev);
          const glm::vec2 world[4] = {outer, inner, inner, outer};
          const float g[4] = {topElev, topElev, faceBot, faceBot};
          const glm::vec2 uv[4] = {
              uvIn(bt.uvRect, face[0]),
              uvIn(bt.uvRect, face[1]),
              uvIn(bt.uvRect, lerpUv(face[1], face[2], t)),
              uvIn(bt.uvRect, lerpUv(face[0], face[3], t))};
          pushQuad(ensure().vertices, world, g, uv, normal);
        };

        // +y (south / screen-left): outer = +x edge corner, inner = +x+ corner.
        emitSide(wx,
                 wy + 1,
                 {fx, fy + 1.0f},
                 {fx + 1.0f, fy + 1.0f},
                 uvs.left,
                 {0.0f, 1.0f, 0.0f});
        // +x (east / screen-right).
        emitSide(wx + 1,
                 wy,
                 {fx + 1.0f, fy + 1.0f},
                 {fx + 1.0f, fy},
                 uvs.right,
                 {1.0f, 0.0f, 0.0f});
      }

  std::vector<VoxelMeshSlice> out;
  out.reserve(buckets.size());
  for (auto& [key, slice] : buckets)
    out.push_back(std::move(slice));
  return out;
}

} // namespace sfs
