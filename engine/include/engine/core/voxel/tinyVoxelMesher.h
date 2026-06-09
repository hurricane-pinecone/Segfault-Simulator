#pragma once

#include "engine/core/voxel/tinyVoxelChunk.h"
#include "engine/core/voxel/tinyVoxelMaterial.h"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include "glm/glm/ext/vector_int3.hpp"

#include <functional>
#include <vector>

namespace sfs
{

// One vertex of a tiny-voxel mesh: WORLD-space position (the GPU transforms it
// with view/proj, unlike the iso mesher which pre-projects), the face normal
// (flat -> faceted Teardown shading), and the per-voxel colour.
struct TinyVoxelVertex
{
  glm::vec3 pos;
  glm::vec3 normal;
  glm::vec4
      color; // rgb + alpha (1 = opaque terrain; < 1 for transparent water)
};

// A chunk's meshed faces, split by render pass: opaque (terrain/structures)
// drawn first with depth write, water drawn after with blending and no depth
// write.
struct TinyChunkMesh
{
  std::vector<TinyVoxelVertex> opaque;
  std::vector<TinyVoxelVertex> water;
};

// Mesh one tiny-voxel chunk into world-space triangles: two per EXPOSED face,
// all six directions (the camera rotates freely, so no iso 3-face cull). Face
// culling is class-aware (see TinyClass): an opaque face is exposed against air
// OR water (so a submerged bed shows through the translucent water); a water
// face is exposed against air only. `chunkOrigin` is the world voxel coord of
// local (0,0,0). `classOutside(wx,wy,wz)` gives the class of a neighbour cell
// OUTSIDE this chunk (in-chunk neighbours are read from `chunk` directly).
inline TinyChunkMesh
meshTinyChunk(glm::ivec3 chunkOrigin,
              const TinyVoxelChunk& chunk,
              const std::function<int(int, int, int)>& classOutside)
{
  struct Face
  {
    glm::ivec3 dir;      // neighbour to test for exposure
    glm::vec3 normal;    // outward normal
    glm::vec3 corner[4]; // unit-cube corners (one quad)
  };

  // Corners wind around each face; culling is disabled at draw time, so the
  // order only needs to form a planar quad (triangulated 0-1-2, 0-2-3).
  static const Face kFaces[6] = {
      {{1, 0, 0},
       {1.0f, 0.0f, 0.0f},
       {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}}},
      {{-1, 0, 0},
       {-1.0f, 0.0f, 0.0f},
       {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}}},
      {{0, 1, 0},
       {0.0f, 1.0f, 0.0f},
       {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}}},
      {{0, -1, 0},
       {0.0f, -1.0f, 0.0f},
       {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}}},
      {{0, 0, 1},
       {0.0f, 0.0f, 1.0f},
       {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}},
      {{0, 0, -1},
       {0.0f, 0.0f, -1.0f},
       {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}}},
  };

  TinyChunkMesh out;

  for (int lz = 0; lz < kTinyChunkSize; ++lz)
    for (int ly = 0; ly < kTinyChunkSize; ++ly)
      for (int lx = 0; lx < kTinyChunkSize; ++lx)
      {
        const std::uint32_t v = chunk.at(lx, ly, lz);
        if (v == 0u)
          continue;

        const bool water = tinyIsWater(tinyMaterialOf(v));
        const float alpha = water ? 0.55f : 1.0f;
        const glm::vec4 color{static_cast<float>((v >> 24) & 0xFFu) / 255.0f,
                              static_cast<float>((v >> 16) & 0xFFu) / 255.0f,
                              static_cast<float>((v >> 8) & 0xFFu) / 255.0f,
                              alpha};
        std::vector<TinyVoxelVertex>& list = water ? out.water : out.opaque;

        const int wx = chunkOrigin.x + lx;
        const int wy = chunkOrigin.y + ly;
        const int wz = chunkOrigin.z + lz;
        const glm::vec3 base{static_cast<float>(wx),
                             static_cast<float>(wy),
                             static_cast<float>(wz)};

        for (const Face& f : kFaces)
        {
          const int nlx = lx + f.dir.x;
          const int nly = ly + f.dir.y;
          const int nlz = lz + f.dir.z;
          int nc;
          if (nlx >= 0 && nlx < kTinyChunkSize && nly >= 0 &&
              nly < kTinyChunkSize && nlz >= 0 && nlz < kTinyChunkSize)
            nc = static_cast<int>(tinyClassOf(chunk.at(nlx, nly, nlz)));
          else
            nc = classOutside(wx + f.dir.x, wy + f.dir.y, wz + f.dir.z);

          if (water)
          {
            if (nc != static_cast<int>(TinyClass::Air))
              continue; // water hidden against water/solid
          }
          else if (nc == static_cast<int>(TinyClass::Opaque))
            continue; // opaque hidden only behind opaque

          const glm::vec3 p0 = base + f.corner[0];
          const glm::vec3 p1 = base + f.corner[1];
          const glm::vec3 p2 = base + f.corner[2];
          const glm::vec3 p3 = base + f.corner[3];

          list.push_back({p0, f.normal, color});
          list.push_back({p1, f.normal, color});
          list.push_back({p2, f.normal, color});
          list.push_back({p0, f.normal, color});
          list.push_back({p2, f.normal, color});
          list.push_back({p3, f.normal, color});
        }
      }

  return out;
}

} // namespace sfs
