#include "../../testHarness.h"

#include <engine/runtime/navigation/collisionSpace.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
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

  TEST("a capsule lying over should have a footprint round all of it")
  {
    // A log 4 m long, 0.3 m round, lying along x at (2, 1).
    CollisionWorld3D::Body log;
    log.id = 9;
    log.position = {2.0f, 1.0f, 0.3f};
    log.rotation =
        glm::angleAxis(0.5f * 3.1415927f, glm::vec3(0.0f, 1.0f, 0.0f));
    log.shapes.push_back(
        {CollisionShape3D::Kind::Capsule, {0.3f, 4.0f, 0.0f}, {}});
    const CollisionWorld3D world(std::span(&log, 1));
    const auto all = footprints(world);
    CHECK(all.size() == 1u);
    if (!all.empty())
    {
      CHECK(all[0].circle);
      CHECK(testing::approx(all[0].half.x, 2.0f, 1e-3f));
      CHECK(testing::approx(all[0].top - all[0].bottom, 0.6f, 1e-3f));
    }
  }

  TEST("the broad phase should answer every query as the exact check does "
       "(fits and what it touches, random poses, probes, tilts, layers), "
       "settling the clear ones without it")
  {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const auto in = [&](float a, float b) { return a + (b - a) * u(rng); };
    // A yard's worth (enough for the grid): boxes yawed and some tipped,
    // trunks, spheres, a log lying over, on several layers.
    std::vector<CollisionWorld3D::Body> bodies;
    for (uint64_t id = 1; id <= 160; ++id)
    {
      const glm::vec2 at{in(-120.0f, 120.0f), in(-80.0f, 80.0f)};
      const int kind = static_cast<int>(id % 5);
      if (kind <= 1)
      {
        auto b = box(id,
                     {at, in(0.0f, 2.0f)},
                     {in(0.1f, 6.0f), in(0.1f, 3.0f), in(0.2f, 3.0f)},
                     in(-3.0f, 3.0f),
                     static_cast<uint32_t>(id % 3));
        if (kind == 1)
          b.rotation = b.rotation * glm::angleAxis(in(-0.6f, 0.6f),
                                                   glm::vec3(1.0f, 0.0f, 0.0f));
        bodies.push_back(b);
      }
      else if (kind == 2)
        bodies.push_back(trunk(id, at, in(0.1f, 0.8f), in(2.0f, 8.0f)));
      else
      {
        CollisionWorld3D::Body b;
        b.id = id;
        b.position = {at, in(0.0f, 1.5f)};
        if (kind == 3)
          b.shapes.push_back({CollisionShape3D::Kind::Sphere,
                              {in(0.1f, 1.0f), 0.0f, 0.0f},
                              {}});
        else
        {
          b.rotation = glm::angleAxis(
              in(0.3f, 1.5f),
              glm::normalize(glm::vec3(in(-1.0f, 1.0f), 1.0f, 0.0f)));
          b.shapes.push_back({CollisionShape3D::Kind::Capsule,
                              {in(0.1f, 0.5f), in(1.0f, 5.0f), 0.0f},
                              {}});
        }
        bodies.push_back(b);
      }
    }
    // And a few parked in a second world (exact, a world of a few).
    std::vector<CollisionWorld3D::Body> parked;
    for (uint64_t id = 500; id < 506; ++id)
      parked.push_back(box(id,
                           {in(-40.0f, 40.0f), in(-30.0f, 30.0f), 1.5f},
                           {6.0f, 2.5f, 3.0f},
                           in(-3.0f, 3.0f)));
    auto world = std::make_shared<const CollisionWorld3D>(bodies);
    auto other = std::make_shared<const CollisionWorld3D>(parked);
    CollisionSpace::Options options;
    options.layers = ~(1u << 2);
    options.ignore = {7, 8, 501};
    // (Rolling ground: the boxes tilt as they stand.)
    options.ground = [](glm::vec2 p)
    { return 0.08f * p.x + 0.6f * std::sin(0.15f * p.y); };
    const CollisionSpace broad({world, other}, options);
    CollisionSpace::Options plain = options;
    plain.broadPhase = false;
    const CollisionSpace exact({world, other}, plain);
    int same = 0, settled = 0, touched = 0, cases = 0;
    const CheckStats before = checkStats();
    for (int probe = 0; probe < 40; ++probe)
    {
      std::vector<Box3> boxes{{in(-4.0f, -0.5f),
                               in(0.5f, 4.0f),
                               in(0.5f, 1.8f),
                               in(-0.3f, 0.3f),
                               in(0.0f, 0.5f),
                               in(1.0f, 4.0f)}};
      if (probe % 2)
        boxes.push_back({in(2.0f, 3.0f),
                         in(3.0f, 5.0f),
                         in(1.0f, 3.5f),
                         0.0f,
                         in(0.0f, 1.0f),
                         in(1.2f, 2.0f)});
      const float grow = in(0.0f, 0.1f), extend = in(0.0f, 1.2f),
                  widen = in(0.0f, 0.3f);
      const auto pb = broad.prepare(boxes, grow, extend, widen);
      const auto pe = exact.prepare(boxes, grow, extend, widen);
      for (int k = 0; k < 250; ++k)
      {
        const Pose2 pose{
            {in(-130.0f, 130.0f), in(-90.0f, 90.0f)}, in(-3.2f, 3.2f)};
        const bool fb = broad.fits(pb.get(), pose);
        const bool fe = exact.fits(pe.get(), pose);
        const auto hb = broad.hit(pb.get(), pose);
        const auto he = exact.hit(pe.get(), pose);
        ++cases;
        same += fb == fe && hb.has_value() == he.has_value() &&
                (!hb || hb->id == he->id);
        touched += !fe;
      }
    }
    const CheckStats& after = checkStats();
    settled = static_cast<int>(after.broad - before.broad);
    CHECK(same == cases);
    // (Both kinds of answer met, and most of the clear ones settled.)
    CHECK(touched > cases / 50);
    CHECK(settled > (cases - touched));
    std::printf("  broad phase: %d of %d poses the same, %d touching, %d "
                "queries settled without Jolt\n",
                same,
                cases,
                touched,
                settled);
  }

  return testing::report("collisionSpaceTests");
}
