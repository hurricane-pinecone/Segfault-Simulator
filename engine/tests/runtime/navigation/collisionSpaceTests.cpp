#include "../../testHarness.h"

#include <engine/runtime/navigation/collisionSpace.h>

#include <cmath>
#include <memory>
#include <vector>

using namespace sfs;
using namespace sfs::nav;

namespace
{
CollisionWorld3D::Body
box(uint64_t id, glm::vec3 at, glm::vec3 size, float yaw, uint32_t layer = 0)
{
  CollisionWorld3D::Body b;
  b.id = id;
  b.layer = layer;
  b.position = at;
  b.rotation = yawRotation(yaw);
  b.shapes.push_back({CollisionShape3D::Kind::Box, size, {0.0f, 0.0f, 0.0f}});
  return b;
}

// A tree's trunk: an upright capsule `radius` round, `height` tall, standing on
// the ground at `at`.
CollisionWorld3D::Body
trunk(uint64_t id, glm::vec2 at, float radius, float height)
{
  CollisionWorld3D::Body b;
  b.id = id;
  b.position = {at, 0.0f};
  b.shapes.push_back({CollisionShape3D::Kind::Capsule,
                      {radius, height, 0.0f},
                      {0.0f, 0.0f, 0.5f * height}});
  return b;
}

const BoxSpace::Solid* find(const std::vector<BoxSpace::Solid>& all,
                            uint64_t id)
{
  for (const BoxSpace::Solid& s : all)
    if (s.id == id)
      return &s;
  return nullptr;
}
} // namespace

int main()
{
  TEST("footprints should be each collider's exact outline: a box yawed, a "
       "trunk's circle, heights over the "
       "ground, by layer")
  {
    const std::vector<CollisionWorld3D::Body> bodies{
        box(1, {4.0f, 2.0f, 0.3f}, {2.0f, 1.0f, 0.6f}, 0.5f),
        trunk(2, {-3.0f, 1.0f}, 0.7f, 4.0f),
        box(3, {0.0f, 9.0f, 1.0f}, {1.0f, 1.0f, 2.0f}, 0.0f, 3)};
    const CollisionWorld3D world(bodies);
    // (The ground 0.1 m up everywhere: heights are over it.)
    const auto all = footprints(world, ~0u, {}, [](glm::vec2) { return 0.1f; });
    CHECK(all.size() == 3u);
    const BoxSpace::Solid* rock = find(all, 1);
    CHECK(rock != nullptr);
    if (rock)
    {
      CHECK(!rock->circle);
      CHECK(testing::approx(rock->centre.x, 4.0f) &&
            testing::approx(rock->centre.y, 2.0f));
      CHECK(testing::approx(rock->half.x, 1.0f, 1e-3f) &&
            testing::approx(rock->half.y, 0.5f, 1e-3f));
      CHECK(testing::approx(
          std::remainder(rock->yaw - 0.5f, 3.1415927f), 0.0f, 1e-3f));
      CHECK(testing::approx(rock->bottom, -0.1f, 1e-3f) &&
            testing::approx(rock->top, 0.5f, 1e-3f));
    }
    const BoxSpace::Solid* tree = find(all, 2);
    CHECK(tree != nullptr);
    if (tree)
    {
      CHECK(tree->circle);
      CHECK(testing::approx(tree->half.x, 0.7f));
      CHECK(testing::approx(tree->bottom, -0.1f, 1e-3f) &&
            testing::approx(tree->top, 3.9f, 1e-3f));
    }
    // Layer 3 counted only when asked for; ignored ids left out.
    CHECK(footprints(world, ~(1u << 3)).size() == 2u);
    const uint64_t skip[] = {2};
    CHECK(footprints(world, ~0u, skip).size() == 2u);
  }

  TEST("a CollisionSpace's footprints and raster should be its colliders' "
       "outlines, and its fits() the same "
       "edges: a box 2 cm off a trunk fits, 2 cm into it doesn't")
  {
    const std::vector<CollisionWorld3D::Body> bodies{
        trunk(7, {0.0f, 0.0f}, 0.5f, 4.0f)};
    auto world = std::make_shared<const CollisionWorld3D>(bodies);
    CollisionSpace::Options options;
    const CollisionSpace space({world}, options);
    const auto all = space.footprints();
    CHECK(all.size() == 1u && all.front().circle &&
          testing::approx(all.front().half.x, 0.5f));
    // A 1 x 1 m box standing 0..1 m: its side 2 cm off the trunk's, then 2 cm
    // into it.
    const Box3 probe{-0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 1.0f};
    const auto prepared = space.prepare(std::span(&probe, 1), 0.0f, 0.0f, 0.0f);
    CHECK(space.fits(prepared.get(), {{1.02f, 0.0f}, 0.0f}));
    CHECK(!space.fits(prepared.get(), {{0.98f, 0.0f}, 0.0f}));
    // The raster (10 cm cells round it): the cell at its middle solid, one 0.6
    // m out clear.
    std::vector<uint8_t> cells;
    space.raster({-1.0f, -1.0f}, 0.1f, 20, 20, 0.0f, cells);
    CHECK(cells[10 * 20 + 10] == 1);
    CHECK(cells[10 * 20 + 16] == 0);
  }

  return testing::report("collisionSpaceTests");
}
