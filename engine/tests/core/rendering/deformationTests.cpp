#include "../../testHarness.h"

#include <engine/core/rendering/deformation.h>
#include <engine/core/rendering/mesh.h>
#include <engine/core/rendering/terrain.h>

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
    Footprint footprint = footprintOf({0, 0}, {4, 1}, -0.05f);
    footprint.size = {5, 1};
    footprint.target.push_back(0.0f); // under it, but no lower than the ground
    map.press(footprint, settings);
    CHECK(near(map.at({-1, 0}), -0.04f));
    CHECK(map.at({4, 0}) == 0.0f);
    CHECK(map.at({2, 1}) > 0.0f); // its side rose
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
    // A square 0.1 above the ground, and another far above it.
    Square low = square(0.15f, 0.1f);
    Square high = square(0.15f, 5.0f);
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
