#include "../../testHarness.h"

#include <engine/core/voxel/voxelMesher.h>

#include "glm/glm/common.hpp"
#include <map>
#include <string>
#include <tuple>

using namespace sfs;

namespace
{
const std::string kTexA = "blockA";
const std::string kTexB = "blockB";

// A sparse voxel view: any position not set is air.
struct TestView : IVoxelView
{
  std::map<std::tuple<int, int, int>, BlockId> solids;

  void put(int x, int y, int z, BlockId id) { solids[{x, y, z}] = id; }

  BlockId blockAt(int x, int y, int z) const override
  {
    const auto it = solids.find({x, y, z});
    return it == solids.end() ? kAirBlock : it->second;
  }
};

// id 1 -> opaque cube (texture A), id 2 -> opaque cube (texture B).
struct TestRegistry : IBlockRegistry
{
  const BlockType& type(BlockId id) const override
  {
    static BlockType air{};
    static BlockType a{BlockShape::Cube,
                       &kTexA,
                       {0, 0, 1, 1},
                       true,
                       true,
                       SurfaceEffect::Type::None};
    static BlockType b{BlockShape::Cube,
                       &kTexB,
                       {0, 0, 1, 1},
                       true,
                       true,
                       SurfaceEffect::Type::None};
    static BlockType slab{BlockShape::Slab,
                          &kTexA,
                          {0, 0, 1, 1},
                          true,
                          true,
                          SurfaceEffect::Type::None};
    if (id == 1)
      return a;
    if (id == 2)
      return b;
    if (id == 3)
      return slab;
    return air;
  }
};

std::size_t totalVerts(const std::vector<VoxelMeshSlice>& slices)
{
  std::size_t n = 0;
  for (const auto& s : slices)
    n += s.vertices.size();
  return n;
}

// Count the emitted faces (6 verts each) whose normal points the given axis.
int faceCount(const std::vector<VoxelMeshSlice>& slices, glm::vec3 normal)
{
  int verts = 0;
  for (const auto& s : slices)
    for (const auto& v : s.vertices)
      if (v.normal == normal)
        ++verts;
  return verts / 6;
}
} // namespace

int main()
{
  TestRegistry reg;

  TEST("a lone block emits exactly the three camera-facing faces")
  {
    TestView view;
    view.put(0, 0, 0, 1);
    const auto slices = meshChunk({0, 0, 0}, view, reg);

    CHECK(totalVerts(slices) == 18);          // 3 faces * 2 tris * 3 verts
    CHECK(faceCount(slices, {0, 0, 1}) == 1); // +z top
    CHECK(faceCount(slices, {0, 1, 0}) == 1); // +y south
    CHECK(faceCount(slices, {1, 0, 0}) == 1); // +x east
  }

  TEST("a solid +z neighbour culls the shared top face")
  {
    TestView view;
    view.put(0, 0, 0, 1);
    view.put(0, 0, 1, 1); // sits on top
    const auto slices = meshChunk({0, 0, 0}, view, reg);

    // lower: +y,+x (top culled); upper: +z,+y,+x -> 5 faces.
    CHECK(totalVerts(slices) == 30);
    CHECK(faceCount(slices, {0, 0, 1}) == 1); // only the upper block's top
  }

  TEST("a solid +x neighbour culls the shared east face")
  {
    TestView view;
    view.put(0, 0, 0, 1);
    view.put(1, 0, 0, 1);
    const auto slices = meshChunk({0, 0, 0}, view, reg);

    // lower-x: +z,+y (east culled); upper-x: +z,+y,+x -> 5 faces.
    CHECK(totalVerts(slices) == 30);
    CHECK(faceCount(slices, {1, 0, 0}) == 1);
  }

  TEST("a block enclosed on +x/+y/+z emits nothing")
  {
    TestView view;
    view.put(0, 0, 0, 1); // enclosed on its three visible sides
    view.put(1, 0, 0, 1); // +x
    view.put(0, 1, 0, 1); // +y
    view.put(0, 0, 1, 1); // +z
    const auto slices = meshChunk({0, 0, 0}, view, reg);

    // Only the three neighbours contribute (3 faces each = 9); the enclosed
    // block adds nothing, or the total would exceed 54.
    CHECK(totalVerts(slices) == 54);
  }

  TEST("a slab is one level tall with its top always exposed")
  {
    TestView view;
    view.put(0, 0, 0, 3); // a lone slab
    const auto slices = meshChunk({0, 0, 0}, view, reg);

    CHECK(totalVerts(slices) == 18); // top (always) + 2 sides

    float topGround = -1.0f;
    float sideMin = 999.0f;
    float sideMax = -1.0f;
    for (const auto& s : slices)
      for (const auto& v : s.vertices)
      {
        if (v.normal == glm::vec3{0.0f, 0.0f, 1.0f})
          topGround = v.ground;
        if (v.normal == glm::vec3{1.0f, 0.0f, 0.0f})
        {
          sideMin = glm::min(sideMin, v.ground);
          sideMax = glm::max(sideMax, v.ground);
        }
      }

    CHECK(topGround == 1.0f); // slab top one level up (a cube would be 2)
    CHECK(sideMin == 0.0f);
    CHECK(sideMax == 1.0f); // side spans exactly one level
  }

  TEST("a slab's top stays exposed even with a solid cell above")
  {
    TestView view;
    view.put(0, 0, 0, 3); // slab
    view.put(0, 0, 1, 1); // cube in the cell above (floats a level higher)
    const auto slices = meshChunk({0, 0, 0}, view, reg);

    int slabTops = 0;
    for (const auto& s : slices)
      for (const auto& v : s.vertices)
        if (v.normal == glm::vec3{0.0f, 0.0f, 1.0f} && v.ground == 1.0f)
          ++slabTops;
    CHECK(slabTops == 6); // the slab's top face still emits (one quad)
  }

  TEST("a cube keeps the part of its face above a shorter slab neighbour")
  {
    TestView view;
    view.put(0, 0, 0, 1); // full cube
    view.put(1, 0, 0, 3); // slab to its +x (covers only the lower level)
    const auto slices = meshChunk({0, 0, 0}, view, reg);

    // The cube's east face must still show its top level [1,2]; without
    // partial-coverage culling the slab would hide the whole face and the
    // tallest east-face vertex would only reach the slab's 1 -> a hole.
    float maxEastGround = -1.0f;
    for (const auto& s : slices)
      for (const auto& v : s.vertices)
        if (v.normal == glm::vec3{1.0f, 0.0f, 0.0f})
          maxEastGround = glm::max(maxEastGround, v.ground);
    CHECK(maxEastGround == 2.0f);
  }

  TEST("distinct materials split into separate slices")
  {
    TestView view;
    view.put(0, 0, 0, 1); // texture A
    view.put(4, 4, 0, 2); // texture B, far apart
    const auto slices = meshChunk({0, 0, 0}, view, reg);

    CHECK(slices.size() == 2);
    for (const auto& s : slices)
      CHECK(s.textureId != nullptr);
  }

  return testing::report("voxelMesherTests");
}
