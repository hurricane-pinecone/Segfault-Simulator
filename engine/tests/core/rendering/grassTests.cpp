#include "../../testHarness.h"

#include <engine/core/rendering/grass.h>
#include <engine/core/rendering/meshPrimitives.h>

#include "glm/glm/ext/matrix_transform.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace sfs;

namespace
{

GrassGround groundOf(const MeshData& mesh,
                     const glm::mat4& model = glm::mat4(1.0f))
{
  std::vector<glm::vec3> positions;
  for (const MeshVertex& vertex : mesh.vertices)
    positions.push_back(vertex.position);
  return GrassGround::build(positions, mesh.indices, model);
}

bool near(float a, float b, float within = 1e-3f)
{
  return std::abs(a - b) <= within;
}

} // namespace

int main()
{
  TEST("a grid should have square texels about the size asked, a capped "
       "number a side")
  {
    const GrassGrid grid = GrassGrid::over({-2.0f, -1.0f}, {2.0f, 1.0f}, 0.25f);
    CHECK(grid.size == glm::ivec2(16, 8));
    CHECK(near(grid.texelSize().x, 0.25f) && near(grid.texelSize().y, 0.25f));
    CHECK(near(grid.center(0, 0).x, -1.875f));
    CHECK(near(grid.toTexels({0.0f, 0.0f}).x, 8.0f));

    const GrassGrid coarse =
        GrassGrid::over({0.0f, 0.0f}, {1000.0f, 10.0f}, 0.25f, 100);
    CHECK(coarse.size.x == 100);
    CHECK(near(coarse.texelSize().x, coarse.texelSize().y));
    CHECK(GrassGrid().empty() && GrassGrid().count() == 0u);
  }

  TEST("the ground should be what faces up, seen from above, highest first")
  {
    // A 4 x 4 plane: all of it, flat, facing up.
    const GrassGround plane = groundOf(meshes::plane({4.0f, 4.0f}));
    CHECK(plane.grid().size == glm::ivec2(16, 16));
    CHECK(plane.groundTexels() == 256u);
    CHECK(plane.has(3, 7));
    CHECK(near(plane.texel(3, 7).z, 1.0f) && near(plane.texel(3, 7).w, 0.0f));

    // A box: just its top (its sides are too steep, its bottom faces down).
    const GrassGround box = groundOf(meshes::box({2.0f, 2.0f, 2.0f}));
    CHECK(box.groundTexels() == 64u);
    CHECK(near(box.texel(4, 4).w, 2.0f)); // it stands on z = 0

    // Turned on its side, the plane faces sideways: no ground.
    const glm::mat4 sideways = glm::rotate(
        glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    CHECK(groundOf(meshes::plane({4.0f, 4.0f}), sideways).empty());

    // Scaled up, its texels stay about as wide in the world: more of them.
    const GrassGround big =
        groundOf(meshes::plane({4.0f, 4.0f}),
                 glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 2.0f, 1.0f)));
    CHECK(big.grid().size == glm::ivec2(32, 32));

    // Nothing to grow on.
    CHECK(groundOf(MeshData{}).empty());
    CHECK(GrassGround().groundTexels() == 0u);
  }

  TEST("a window should sit on the mesh's texel lattice around its middle, "
       "coarser if it must, cut to the mesh")
  {
    const GrassGrid window = grassWindow(
        {-100.0f, -100.0f}, {100.0f, 100.0f}, {3.1f, 0.0f}, 10.0f, 0.25f);
    CHECK(near(window.texelSize().x, 0.25f));
    // Its corner on a multiple of the texel, the square around the middle
    // inside it.
    CHECK(near(window.min.x / 0.25f, std::round(window.min.x / 0.25f)));
    CHECK(window.min.x <= -6.9f && window.max.x >= 13.1f);
    CHECK(window.min.y <= -10.0f && window.max.y >= 10.0f);
    CHECK(window.size.x <= 84 && window.size.y <= 84);

    // Moved on, the texels it shares with the last are the same.
    const GrassGrid moved = grassWindow(
        {-100.0f, -100.0f}, {100.0f, 100.0f}, {7.3f, 2.0f}, 10.0f, 0.25f);
    const glm::vec2 shift = (moved.min - window.min) / 0.25f;
    CHECK(near(shift.x, std::round(shift.x)) &&
          near(shift.y, std::round(shift.y)));

    // Too wide for kGrassMaxTexels at that texel: twice (or four times...) as
    // coarse, still on the lattice.
    const GrassGrid wide = grassWindow(
        {-1000.0f, -1000.0f}, {1000.0f, 1000.0f}, {0.0f, 0.0f}, 200.0f, 0.25f);
    CHECK(wide.size.x <= kGrassMaxTexels && near(wide.texelSize().x, 0.5f));
    CHECK(near(wide.min.x, -200.0f));

    // Cut to the mesh; nothing off it.
    const GrassGrid edge = grassWindow(
        {-100.0f, -100.0f}, {100.0f, 100.0f}, {95.0f, 0.0f}, 10.0f, 0.25f);
    CHECK(near(edge.max.x, 100.0f));
    CHECK(
        grassWindow(
            {-100.0f, -100.0f}, {100.0f, 100.0f}, {500.0f, 0.0f}, 10.0f, 0.25f)
            .empty());
  }

  TEST("a window's ground should be just the mesh's on it")
  {
    const MeshData mesh = meshes::plane({40.0f, 40.0f});
    std::vector<glm::vec3> positions;
    for (const MeshVertex& vertex : mesh.vertices)
      positions.push_back(vertex.position);
    const GrassGrid window = grassWindow(
        {-20.0f, -20.0f}, {20.0f, 20.0f}, {4.0f, 4.0f}, 2.0f, 0.25f);
    const GrassGround ground = GrassGround::build(
        positions, mesh.indices, glm::mat4(1.0f), {}, &window);
    CHECK(ground.grid() == window);
    CHECK(ground.groundTexels() == window.count());
    // A window off the mesh: none.
    const GrassGrid off{{50.0f, 50.0f}, {52.0f, 52.0f}, {8, 8}};
    CHECK(GrassGround::build(positions, mesh.indices, glm::mat4(1.0f), {}, &off)
              .empty());
  }

  TEST("maps should start full, and change by stamps, sets and fills")
  {
    GrassMaps maps(GrassGrid::over({-5.0f, -5.0f}, {5.0f, 5.0f}, 0.25f));
    CHECK(!maps.empty());
    CHECK(maps.sample(GrassMaps::Coverage, {1.0f, 1.0f}) == 1.0f);
    CHECK(maps.sample(GrassMaps::Length, {1.0f, 1.0f}) == 1.0f);
    CHECK(maps.sample(GrassMaps::Data0, {1.0f, 1.0f}) == 0.0f);
    CHECK(maps.sample(GrassMaps::Coverage, {50.0f, 0.0f}) == 1.0f); // off it
    CHECK(GrassMaps::name(GrassMaps::Data2) == "data2");

    // All of it changed, to start with; then nothing until it is.
    auto [from, to] = maps.takeChanged();
    CHECK(from == glm::ivec2(0) && to == maps.grid().size);
    std::tie(from, to) = maps.takeChanged();
    CHECK(from == to);

    // A hard stamp, toward 0: bare inside, untouched outside; only its
    // square changed.
    maps.stamp(GrassMaps::Coverage,
               {0.0f, 0.0f},
               1.0f,
               0.0f,
               GrassMaps::Blend::Toward,
               1.0f,
               0.0f);
    CHECK(maps.sample(GrassMaps::Coverage, {0.0f, 0.0f}) == 0.0f);
    CHECK(maps.sample(GrassMaps::Coverage, {2.0f, 0.0f}) == 1.0f);
    std::tie(from, to) = maps.takeChanged();
    CHECK(from.x >= 15 && to.x <= 25 && from.y >= 15 && to.y <= 25);
    CHECK(to.x > from.x);

    // Soft and gentle: part way at its middle, less toward its edge.
    maps.stamp(GrassMaps::Data0,
               {0.0f, 0.0f},
               2.0f,
               1.0f,
               GrassMaps::Blend::Toward,
               0.5f,
               1.0f);
    const float middle = maps.sample(GrassMaps::Data0, {0.0f, 0.0f});
    const float edge = maps.sample(GrassMaps::Data0, {1.6f, 0.0f});
    CHECK(middle > 0.4f && middle < 0.6f);
    CHECK(edge > 0.0f && edge < middle);

    // Adding, and raising and lowering only.
    maps.stamp(GrassMaps::Data0,
               {0.0f, 0.0f},
               0.3f,
               0.2f,
               GrassMaps::Blend::Add,
               1.0f,
               0.0f);
    CHECK(near(
        maps.sample(GrassMaps::Data0, {0.0f, 0.0f}), middle + 0.2f, 0.01f));
    maps.stamp(GrassMaps::Data0,
               {0.0f, 0.0f},
               0.3f,
               0.1f,
               GrassMaps::Blend::Max,
               1.0f,
               0.0f);
    CHECK(near(
        maps.sample(GrassMaps::Data0, {0.0f, 0.0f}), middle + 0.2f, 0.01f));
    maps.stamp(GrassMaps::Data0,
               {0.0f, 0.0f},
               0.3f,
               0.1f,
               GrassMaps::Blend::Min,
               1.0f,
               0.0f);
    CHECK(near(maps.sample(GrassMaps::Data0, {0.0f, 0.0f}), 0.1f, 0.01f));

    // A brush too gentle to move a step still gets there, a step at a time.
    GrassMaps slow(GrassGrid::over({0.0f, 0.0f}, {1.0f, 1.0f}, 0.25f));
    slow.stamp(GrassMaps::Length,
               {0.5f, 0.5f},
               2.0f,
               0.0f,
               GrassMaps::Blend::Toward,
               0.001f,
               0.0f);
    CHECK(slow.at(GrassMaps::Length, 1, 1) < 1.0f);

    // Texels and whole channels; the GPU's images.
    maps.set(GrassMaps::Length, 2, 3, 0.5f);
    CHECK(near(maps.at(GrassMaps::Length, 2, 3), 0.5f, 0.01f));
    maps.fill(GrassMaps::Data3, 1.0f);
    CHECK(maps.sample(GrassMaps::Data3, {-4.0f, 4.0f}) == 1.0f);
    std::vector<uint8_t> shape, data;
    maps.pixels({2, 3}, {4, 4}, shape, data);
    CHECK(shape.size() == 8u && data.size() == 8u);
    CHECK(shape[0] == 255 && std::abs(int(shape[1]) - 128) <= 1);
    CHECK(data[3] == 255);
  }

  TEST("maps should survive saving, compactly, and refuse what isn't them")
  {
    GrassMaps maps(GrassGrid::over({-5.0f, -2.0f}, {5.0f, 2.0f}, 0.25f));
    CHECK(maps.toJson().size() == 1u); // all default: just the grid
    maps.stamp(GrassMaps::Coverage, {0.0f, 0.0f}, 1.5f, 0.0f);
    maps.stamp(GrassMaps::Data1, {3.0f, 0.0f}, 1.0f, 0.7f);
    const nlohmann::json saved = maps.toJson();
    CHECK(saved.contains("coverage") && saved.contains("data1"));
    CHECK(!saved.contains("length"));
    CHECK(saved.dump().size() < 600u); // runs, not every texel
    CHECK(GrassMaps::fromJson(saved) == maps);

    CHECK(GrassMaps().toJson() == nlohmann::json::object());
    CHECK(GrassMaps::fromJson(nlohmann::json::object()).empty());
    nlohmann::json broken = saved;
    broken["coverage"] = "not base64!";
    CHECK(GrassMaps::fromJson(broken).empty());
    broken = saved;
    broken["grid"]["size"] = {40000, 1};
    CHECK(GrassMaps::fromJson(broken).empty());

    // A painted density grid from before maps had channels: its coverage.
    const nlohmann::json legacy = {{"min", {0.0f, 0.0f}},
                                   {"max", {1.0f, 1.0f}},
                                   {"size", {2, 2}},
                                   {"cells", "BAA="}}; // 4 texels of 0
    const GrassMaps old = GrassMaps::fromJson(legacy);
    CHECK(old.grid().size == glm::ivec2(2, 2));
    CHECK(old.at(GrassMaps::Coverage, 1, 1) == 0.0f);
    CHECK(old.at(GrassMaps::Length, 1, 1) == 1.0f);
  }

  TEST("grass should flatten under a bender, the way it's going, and spring "
       "back")
  {
    const GrassGround ground = groundOf(meshes::plane({10.0f, 10.0f}));
    GrassTrample trample(ground, glm::mat4(1.0f), 0.35f);
    CHECK(trample.size().x > 0);

    // A ball rolling along +x, sitting on the ground at the origin.
    const GrassBender ball{{0.0f, 0.0f, 0.5f}, 0.5f, {2.0f, 0.0f, 0.0f}};
    CHECK(trample.update(0.0f, {&ball, 1}, 3.0f));
    CHECK(trample.flattened({0.0f, 0.0f}) > 0.95f);
    CHECK(trample.flattened({3.0f, 3.0f}) == 0.0f);
    CHECK(trample.direction({0.1f, 0.0f}).x > 0.7f); // pressed along its way
    // Behind it and beside it too: laid the way it's going, not outward.
    CHECK(trample.direction({-0.35f, 0.0f}).x > 0.9f);
    CHECK(trample.direction({0.0f, 0.35f}).x > 0.8f);
    CHECK(trample.direction({0.0f, 0.35f}).y > 0.0f); // fanned a little out

    // Walking on over grass it already laid down, it lays it its way.
    GrassTrample turned(ground, glm::mat4(1.0f), 0.35f);
    const GrassBender east{{0.0f, 0.0f, 0.5f}, 0.5f, {2.0f, 0.0f, 0.0f}};
    const GrassBender north{{0.0f, 0.0f, 0.5f}, 0.5f, {0.0f, 2.0f, 0.0f}};
    turned.update(0.0f, {&east, 1}, 3.0f);
    turned.update(0.1f, {&north, 1}, 3.0f);
    CHECK(turned.direction({0.0f, -0.2f}).y > 0.9f);

    // Standing still: pushed outward.
    GrassTrample still(ground, glm::mat4(1.0f), 0.35f);
    const GrassBender standing{{0.0f, 0.0f, 0.5f}, 0.5f, {}};
    still.update(0.0f, {&standing, 1}, 3.0f);
    CHECK(still.direction({-0.3f, 0.0f}).x < -0.9f);
    CHECK(still.direction({0.0f, 0.3f}).y > 0.9f);

    // Half the recovery time later, half up; after it all, fully.
    CHECK(trample.update(1.5f, {}, 3.0f));
    CHECK(std::abs(trample.flattened({0.0f, 0.0f}) - 0.5f) < 0.06f);
    trample.update(2.0f, {}, 3.0f);
    CHECK(trample.flattened({0.0f, 0.0f}) == 0.0f);
    CHECK(!trample.update(1.0f, {}, 3.0f)); // nothing left to change

    // Above the grass, or off the field: nothing.
    const GrassBender flying{{0.0f, 0.0f, 2.0f}, 0.5f, {}};
    const GrassBender away{{40.0f, 0.0f, 0.5f}, 0.5f, {}};
    const GrassBender both[] = {flying, away};
    CHECK(!trample.update(0.0f, both, 3.0f));

    // Moved with its entity: the ground's where it now is.
    GrassTrample moved(
        ground, glm::translate(glm::mat4(1.0f), {20.0f, 0.0f, 1.0f}), 0.35f);
    const GrassBender there{{20.0f, 0.0f, 1.5f}, 0.5f, {}};
    moved.update(0.0f, {&there, 1}, 3.0f);
    CHECK(moved.flattened({20.0f, 0.0f}) > 0.95f);

    // The image: direction in rg, how flat in b.
    trample.update(0.0f, {&ball, 1}, 3.0f);
    const std::vector<uint8_t> image = trample.image();
    CHECK(image.size() == std::size_t(trample.size().x) * trample.size().y * 4);
    CHECK(*std::max_element(image.begin(), image.end()) == 255);
  }

  TEST("no ground should press nothing")
  {
    GrassTrample trample(GrassGround{}, glm::mat4(1.0f), 0.3f);
    const GrassBender ball{{0.0f, 0.0f, 0.5f}, 0.5f, {}};
    CHECK(!trample.update(1.0f, {&ball, 1}, 3.0f));
    CHECK(trample.image().empty());
  }

  return testing::report("grassTests");
}
