#include "../../testHarness.h"

#include <engine/core/rendering/deformation.h>
#include <engine/core/rendering/mesh.h>
#include <engine/core/rendering/terrain.h>
#include <glm/glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace sfs;

namespace
{

constexpr float kNone = std::numeric_limits<float>::quiet_NaN();

bool near(float a, float b, float tolerance = 1e-4f)
{
  return std::abs(a - b) <= tolerance;
}

// A footprint `size` texels from `from`, every texel pressed to `target`.
Footprint footprintOf(glm::ivec2 from, glm::ivec2 size, float target)
{
  Footprint out;
  out.from = from;
  out.size = size;
  out.target.assign(std::size_t(size.x) * size.y, target);
  return out;
}

// A flat square, `half` either side of the origin at height `z`, two
// triangles, UVs over it.
struct Square
{
  std::vector<glm::vec3> positions;
  std::vector<uint32_t> indices{0, 1, 2, 0, 2, 3};
  std::vector<glm::vec2> uvs{{0, 1}, {1, 1}, {1, 0}, {0, 0}};
};

Square square(float half, float z = 0.0f)
{
  Square out;
  out.positions = {
      {-half, -half, z}, {half, -half, z}, {half, half, z}, {-half, half, z}};
  return out;
}

// The sum of every offset in a texel rectangle.
float sumOver(const DeformationMap& map, glm::ivec2 from, glm::ivec2 to)
{
  float sum = 0.0f;
  for (int y = from.y; y <= to.y; ++y)
    for (int x = from.x; x <= to.x; ++x)
      sum += map.at({x, y});
  return sum;
}

} // namespace

int main()
{
  TEST("an untouched map should be flat everywhere and hold no tiles")
  {
    DeformationMap map;
    CHECK(map.empty());
    CHECK(map.at({10, -3}) == 0.0f);
    CHECK(map.sample({-4.2f, 7.1f}) == 0.0f);
    map.set({5, 5}, 0.0f); // flat: no tile made
    CHECK(map.empty());
  }

  TEST("texels should land in the right tiles either side of the origin")
  {
    DeformationMap map(0.5f);
    CHECK(map.texelOf({-0.1f, 0.1f}) == glm::ivec2(-1, 0));
    CHECK(DeformationMap::tileOf({-1, 63}) == glm::ivec2(-1, 0));
    CHECK(DeformationMap::tileOf({64, -64}) == glm::ivec2(1, -1));
    CHECK(DeformationMap::tileOf({-65, 0}) == glm::ivec2(-2, 0));
    map.set({-1, -1}, -0.1f);
    map.set({64, 0}, -0.2f);
    CHECK(map.tileCount() == 2);
    CHECK(near(map.at({-1, -1}), -0.1f));
    CHECK(near(map.at({64, 0}), -0.2f));
    CHECK(near(map.deepest(), -0.2f));
  }

  TEST("a block of tiles should hold one of the map's only if it covers it")
  {
    DeformationMap map(0.5f);
    CHECK(!map.anyTileIn({-4, -4}, {8, 8})); // empty
    map.set({-1, -1}, -0.1f);                // tile (-1, -1)
    map.set({64, 0}, -0.2f);                 // tile (1, 0)
    CHECK(map.anyTileIn({-1, -1}, {1, 1}));
    CHECK(map.anyTileIn({1, 0}, {1, 1}));
    CHECK(map.anyTileIn({-4, -4}, {8, 8}));
    CHECK(!map.anyTileIn({0, 0}, {1, 1}));
    CHECK(!map.anyTileIn({2, 0}, {4, 4}));
    CHECK(!map.anyTileIn({-1, -1}, {0, 4})); // spans nothing
    // Blocks smaller than the map (looked up tile by tile) as well as bigger
    // (the map's tiles checked), with the same answers as listing tiles().
    for (int i = 0; i < 6; ++i)
      map.set({i * 64 + 1, 200}, -0.05f); // tiles (0..5, 3)
    for (const glm::ivec2 first :
         {glm::ivec2(-2, -2), glm::ivec2(0, 0), glm::ivec2(3, 3)})
      for (const glm::ivec2 count : {glm::ivec2(1, 1),
                                     glm::ivec2(2, 1),
                                     glm::ivec2(1, 4),
                                     glm::ivec2(10, 10)})
      {
        bool listed = false;
        for (const glm::ivec2 tile : map.tiles())
        {
          const glm::ivec2 at = tile - first;
          listed = listed ||
                   (at.x >= 0 && at.y >= 0 && at.x < count.x && at.y < count.y);
        }
        CHECK(map.anyTileIn(first, count) == listed);
      }
  }

  TEST("sampling should be bilinear between texel centres")
  {
    DeformationMap map(1.0f);
    map.set({0, 0}, -1.0f);
    CHECK(near(map.sample({0.5f, 0.5f}), -1.0f)); // its centre
    CHECK(near(map.sample({1.0f, 0.5f}), -0.5f)); // halfway to the next
    CHECK(near(map.sample({1.5f, 0.5f}), 0.0f));
  }

  TEST("pressing should lower only texels above the target")
  {
    DeformationMap map(0.1f);
    map.set({1, 0}, -0.2f); // deeper already
    PressSettings flat;
    flat.ridge = 0.0f;
    flat.steepest = 100.0f; // no slumping: pressing alone
    const float pressed = map.press(footprintOf({0, 0}, {3, 1}, -0.05f), flat);
    CHECK(near(map.at({0, 0}), -0.05f));
    CHECK(near(map.at({1, 0}), -0.2f)); // not raised back
    CHECK(near(map.at({2, 0}), -0.05f));
    CHECK(near(pressed, 2 * 0.05f * 0.01f, 1e-6f)); // two texels' volume
  }

  TEST("pressing should go no deeper than kDeepest")
  {
    DeformationMap map;
    PressSettings flat;
    flat.ridge = 0.0f;
    map.press(footprintOf({0, 0}, {1, 1}, -5.0f), flat);
    CHECK(near(map.at({0, 0}), -kDeepest));
  }

  TEST("pressed-out soil should rise in a ridge around the footprint")
  {
    DeformationMap map(0.03f);
    PressSettings settings;
    settings.ridge = 0.5f;
    settings.ridgeWidth = 0.09f; // three texels
    settings.ridgeMost = 1.0f;   // no cap
    settings.steepest = 100.0f;  // no slumping: the ridge alone
    map.press(footprintOf({0, 0}, {10, 10}, -0.05f), settings);
    // What rose (outside it) is half of what was pressed out.
    const float down = sumOver(map, {0, 0}, {9, 9});
    const float total = sumOver(map, {-4, -4}, {13, 13});
    CHECK(near(down, -100 * 0.05f));
    CHECK(near(total - down, 0.5f * 100 * 0.05f, 1e-3f));
    CHECK(map.at({-1, 5}) > map.at({-3, 5})); // most near the edge
    CHECK(map.at({-6, 5}) == 0.0f);           // nothing past its reach
  }

  TEST("a moving deformer should push its ridge to the sides, not ahead")
  {
    DeformationMap map(0.03f);
    PressSettings settings;
    settings.ridgeMost = 1.0f;
    settings.motion = {1.0f, 0.0f};
    map.press(footprintOf({0, 0}, {6, 6}, -0.05f), settings);
    CHECK(map.at({3, -1}) > 2.0f * map.at({6, 3})); // beside it, ahead of it
  }

  TEST("a ridge shouldn't rise into a rut or under the deformer")
  {
    DeformationMap map(0.03f);
    map.set({-1, 0}, -0.04f); // a rut beside it
    PressSettings settings;
    settings.ridgeMost = 1.0f;
    settings.steepest = 100.0f; // no slumping: the ridge alone
    Footprint footprint = footprintOf({0, 0}, {4, 1}, -0.05f);
    footprint.size = {5, 1};
    footprint.target.push_back(0.0f); // under it, but no lower than the ground
    map.press(footprint, settings);
    CHECK(near(map.at({-1, 0}), -0.04f));
    CHECK(map.at({4, 0}) == 0.0f);
    CHECK(map.at({2, 1}) > 0.0f); // its side rose
  }

  TEST("a press should change the ground only around what it pressed")
  {
    // A wide footprint (a tine sliding on: most of it on ground it's pressed
    // already) pressing two texels: the ridge and slumping stay round those,
    // and an old wall elsewhere under it is left as it stands.
    DeformationMap map(0.03f);
    map.set({30, 4}, -0.2f); // an old step, far steeper than soil stands
    PressSettings settings;
    settings.ridgeMost = 1.0f;
    Footprint footprint = footprintOf({0, 0}, {40, 10}, kNone);
    for (const glm::ivec2 t : {glm::ivec2(2, 4), glm::ivec2(2, 5)})
      footprint.target[std::size_t(t.y) * 40 + t.x] = -0.05f;
    map.press(footprint, settings);
    CHECK(map.at({2, 4}) < -0.02f);
    CHECK(map.at({2, 2}) > 0.0f);        // its ridge, beside it
    CHECK(near(map.at({30, 4}), -0.2f)); // the old step, as it was
    CHECK(map.at({31, 4}) == 0.0f);
    CHECK(map.at({20, 4}) == 0.0f); // nothing out of its reach
  }

  TEST("a stamp should press its shape in from the ground where it lands")
  {
    DeformationMap map(0.01f);
    map.set(map.texelOf({0.0f, 0.0f}), 0.02f); // on a ridge
    PressSettings settings;
    settings.ridge = 0.0f;
    map.stamp({0.0f, 0.0f}, 0.0f, {0.3f, 0.1f}, 0.03f, nullptr, settings);
    CHECK(map.at(map.texelOf({0.0f, 0.0f})) < 0.0f);
    CHECK(map.at(map.texelOf({0.12f, 0.0f})) < 0.0f);  // along its length
    CHECK(map.at(map.texelOf({0.0f, 0.08f})) == 0.0f); // past its width
  }

  TEST("a stamp's shape should come from an image's red, its top ahead")
  {
    // 2 x 2: only the top left deep.
    TextureData shape;
    shape.width = shape.height = 2;
    shape.rgba = {255, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255};
    shape.nearest = true;
    DeformationMap map(0.01f);
    PressSettings settings;
    settings.ridge = 0.0f;
    // Heading +x: its top ahead (+x), its left to +y.
    map.stamp({0.0f, 0.0f}, 0.0f, {0.4f, 0.4f}, 0.05f, &shape, settings);
    CHECK(map.at(map.texelOf({0.15f, 0.15f})) < -0.04f);
    CHECK(map.at(map.texelOf({-0.15f, -0.15f})) == 0.0f);
  }

  TEST("relaxing should round edges off, and fading take them away")
  {
    DeformationMap map(0.1f);
    map.set({0, 0}, -0.09f);
    map.relax(1.0f);
    CHECK(near(map.at({0, 0}), -0.01f));
    CHECK(near(map.at({1, 1}), -0.01f)); // spread to its neighbours
    for (int i = 0; i < 50; ++i)
      map.relax(0.0f, 0.5f);
    CHECK(map.empty()); // all but flat: gone
  }

  TEST("relaxing some tiles should weather only them, by their corners")
  {
    DeformationMap map(0.1f);
    map.set({0, 0}, -0.09f);
    map.set({130, 0}, -0.09f); // tile (2, 0)
    map.set({200, 0}, -0.09f); // tile (3, 0)
    const glm::ivec2 tiles[] = {{0, 0}, {3, 0}};
    const glm::vec4 corners[] = {glm::vec4(1.0f), glm::vec4(0.0f)};
    map.relax(tiles, corners);
    CHECK(near(map.at({0, 0}), -0.01f));
    CHECK(near(map.at({1, 1}), -0.01f));
    CHECK(near(map.at({130, 0}), -0.09f)); // not asked
    CHECK(near(map.at({200, 0}), -0.09f)); // dry: as it was
  }

  TEST("clearing should flatten inside the region only")
  {
    DeformationMap map(0.1f);
    for (int x = 0; x < 20; ++x)
      map.set({x, 0}, -0.05f);
    map.clear({0.0f, -1.0f}, {1.0f, 1.0f});
    CHECK(map.at({5, 0}) == 0.0f);
    CHECK(near(map.at({15, 0}), -0.05f));
    // A triangle's inside only.
    const glm::vec2 triangle[] = {{1.0f, -1.0f}, {2.0f, -1.0f}, {1.0f, 1.0f}};
    map.clear(triangle);
    CHECK(map.at({11, 0}) == 0.0f);
    CHECK(near(map.at({19, 0}), -0.05f));
    map.clearAll();
    CHECK(map.empty());
  }

  TEST("changed tiles should be told once")
  {
    DeformationMap map;
    map.set({0, 0}, -0.01f);
    map.set({1, 0}, -0.01f);
    map.set({100, 0}, -0.01f);
    CHECK(map.changed());
    CHECK(map.takeChanged().size() == 2);
    CHECK(!map.changed());
    CHECK(map.takeChanged().empty());
  }

  TEST("a map saved and loaded should come back to the millimetre")
  {
    DeformationMap map;
    for (int x = -70; x < 70; ++x)
      map.set({x, x / 3}, -0.001f * (x % 37) - 0.004f);
    map.set({5, 200}, 0.012f); // a ridge
    const std::vector<uint8_t> bytes = map.save();
    DeformationMap back;
    CHECK(back.load(bytes));
    CHECK(back.tileCount() == map.tileCount());
    CHECK(near(back.texel(), map.texel()));
    bool same = true;
    for (int x = -70; x < 70; ++x)
      same = same && near(back.at({x, x / 3}), map.at({x, x / 3}), 6e-4f);
    CHECK(same);
    CHECK(near(back.at({5, 200}), 0.012f, 6e-4f));
    // Small: far less than a float a texel of its tiles.
    CHECK(bytes.size() < map.tileCount() * 64 * 64);
    // Not a map: refused, left empty.
    const std::vector<uint8_t> junk = {1, 2, 3, 4, 5, 6, 7};
    CHECK(!back.load(junk));
    CHECK(back.empty());
  }

  TEST("the lowest offset over a coarser grid should keep a narrow rut")
  {
    DeformationMap map(0.03f);
    map.set(map.texelOf({0.6f, 0.6f}), -0.06f); // one texel deep
    const std::vector<float> lowest =
        map.lowestOver({0.0f, 0.0f}, 0.25f, {4, 4});
    CHECK(near(lowest[2 * 4 + 2], -0.06f)); // the 0.25 m texel it's in
    CHECK(lowest[0] == 0.0f);
  }

  TEST("a ground surface should give its height and UVs under a point")
  {
    const Square ground = square(10.0f, 2.0f);
    const GroundSurface surface(
        ground.positions, ground.indices, ground.uvs, glm::mat4(1.0f));
    const auto at = surface.at({5.0f, 0.0f});
    CHECK(at.has_value());
    CHECK(near(at->height, 2.0f));
    CHECK(near(at->uv.x, 0.75f));
    CHECK(!surface.at({12.0f, 0.0f}).has_value()); // off it
    const std::vector<glm::vec4> laid =
        surface.raster({-11.0f, -1.0f}, 1.0f, {22, 2});
    CHECK(laid[0].w == 0.0f);     // off it
    CHECK(near(laid[5].x, 2.0f)); // on it
    CHECK(laid[5].w == 1.0f);
  }

  TEST("an underside should be a mesh's lowest points, below the height only")
  {
    // A square 0.1 above the ground, and another far above it (their edges
    // off texel boundaries, where rounding could take in a texel either way).
    Square low = square(0.145f, 0.1f);
    Square high = square(0.145f, 5.0f);
    const Underside under =
        underside(low.positions, low.indices, glm::mat4(1.0f), 0.5f, 0.03f);
    CHECK(!under.empty());
    CHECK(under.size.x >= 10 && under.size.x <= 11);
    bool flat = true;
    for (float z : under.bottom)
      flat = flat && (std::isnan(z) || near(z, 0.1f));
    CHECK(flat);
    CHECK(
        underside(high.positions, high.indices, glm::mat4(1.0f), 0.5f).empty());
    const Underside both = merged(under, Underside{});
    CHECK(both.size == under.size);
    (void)kNone;
  }

  TEST("a mesh made ready should give the same underside as the mesh itself")
  {
    // A closed box (its faces anticlockwise from outside) and an open square
    // under it, turned and tilted a little.
    std::vector<glm::vec3> positions;
    std::vector<uint32_t> indices;
    const glm::vec3 low(-0.2f, -0.1f, 0.05f), high(0.2f, 0.1f, 0.3f);
    for (int k = 0; k < 8; ++k)
      positions.push_back({k & 1 ? high.x : low.x,
                           k & 2 ? high.y : low.y,
                           k & 4 ? high.z : low.z});
    for (const uint32_t i : {0u, 2u, 3u, 0u, 3u, 1u, 4u, 5u, 7u, 4u, 7u, 6u,
                             0u, 1u, 5u, 0u, 5u, 4u, 2u, 6u, 7u, 2u, 7u, 3u,
                             0u, 4u, 6u, 0u, 6u, 2u, 1u, 3u, 7u, 1u, 7u, 5u})
      indices.push_back(i);
    Square open = square(0.3f, 0.02f);
    for (const uint32_t i : open.indices)
      indices.push_back(i + 8);
    positions.insert(
        positions.end(), open.positions.begin(), open.positions.end());
    const glm::mat4 model = glm::rotate(
        glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, -2.0f, 0.0f)),
        0.3f,
        glm::normalize(glm::vec3(0.1f, 0.05f, 1.0f)));
    const UndersideMesh ready(positions, indices);
    CHECK(!ready.empty());
    for (const float below : {0.5f, 0.1f})
    {
      const Underside a = underside(positions, indices, model, below, 0.03f);
      const Underside b = underside(ready, model, below, 0.03f);
      CHECK(!a.empty());
      CHECK(a.from == b.from && a.size == b.size);
      bool same = a.bottom.size() == b.bottom.size();
      for (std::size_t i = 0; same && i < a.bottom.size(); ++i)
        same = std::isnan(a.bottom[i]) == std::isnan(b.bottom[i]) &&
               (std::isnan(a.bottom[i]) || near(a.bottom[i], b.bottom[i])) &&
               near(a.coverage[i], b.coverage[i]);
      CHECK(same);
    }
    CHECK(underside(
              ready, glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 9)), 0.5f)
              .empty());
    // Its lowest point placed: exactly its lowest vertex's, however it's
    // turned (not its bounds' corners, which a turned wheel swings lower).
    float lowest = std::numeric_limits<float>::infinity();
    for (const glm::vec3& p : positions)
      lowest = std::min(lowest, glm::vec3(model * glm::vec4(p, 1.0f)).z);
    CHECK(near(lowestPoint(ready, model), lowest));
  }

  TEST("half floats should round-trip the offsets the GPU reads")
  {
    CHECK(toHalf(0.0f) == 0);
    CHECK(toHalf(1.0f) == 0x3c00);
    CHECK(toHalf(-2.0f) == 0xc000);
  }

  TEST("a terrain asset should keep whether it's deformable")
  {
    TerrainAsset asset;
    asset.deformable = true;
    CHECK(TerrainAsset::fromJson(asset.toJson()).deformable);
    CHECK(!TerrainAsset::fromJson(nlohmann::json::object()).deformable);
  }

  return testing::report("deformationTests");
}
