#include "../../testHarness.h"
#include "mockRenderer.h"

#include <engine/core/components/meshRendererComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/hierarchy/hierarchy.h>
#include <engine/core/rendering/meshPrimitives.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/rendering/meshLibrary.h>
#include <engine/runtime/rendering/picking.h>
#include <engine/runtime/sceneManager/scene.h>

using namespace sfs;
using sfs::testing3d::MockRenderer;

namespace
{
struct World
{
  AssetStore store;
  MockRenderer renderer;
  Scene scene{0, SceneServices{.assetStore = store, .renderer = &renderer}};
  MeshLibrary meshes{renderer};
  MeshHandle cube = meshes.add(meshes::box({1, 1, 1}));
  MeshHandle floor = meshes.add(meshes::box({20, 20, 0.1f}));

  Entity spawn(MeshHandle mesh, const glm::vec3& position)
  {
    Entity entity = scene.createEntity();
    entity.addComponent<TransformComponent>(TransformComponent{position});
    if (mesh.valid())
      entity.addComponent<MeshRendererComponent>(MeshRendererComponent{mesh});
    return entity;
  }
};

// Looking straight down at (x, y).
Ray downAt(float x, float y) { return Ray{{x, y, 50.0f}, {0.0f, 0.0f, -1.0f}}; }
} // namespace

int main()
{
  TEST("the nearest entity under the ray should be picked")
  {
    World world;
    world.spawn(world.floor, {0, 0, 0});
    Entity box = world.spawn(world.cube, {2, 0, 0.1f});

    const auto hit = pickEntity(world.scene, world.meshes, downAt(2.0f, 0.0f));
    CHECK(hit.has_value());
    CHECK(hit->entity.getId() == box.getId());
    CHECK(testing::approx(hit->distance, 50.0f - 1.1f));
  }

  TEST("missing every entity should pick nothing")
  {
    World world;
    world.spawn(world.cube, {0, 0, 0});
    CHECK(!pickEntity(world.scene, world.meshes, downAt(30.0f, 30.0f))
               .has_value());
  }

  TEST("inside several boxes, the smallest should win")
  {
    World world;
    world.spawn(world.floor, {0, 0, -0.05f});
    Entity box = world.spawn(world.cube, {0, 0, -0.5f});

    // Starts inside both the floor and the box.
    const Ray ray{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}};
    const auto hit = pickEntity(world.scene, world.meshes, ray);
    CHECK(hit->entity.getId() == box.getId());
  }

  TEST("entities without a mesh should be pickable by a marker")
  {
    World world;
    Entity light = world.spawn(MeshHandle{}, {5, 5, 2});

    const auto hit = pickEntity(world.scene, world.meshes, downAt(5.1f, 5.0f));
    CHECK(hit->entity.getId() == light.getId());
    CHECK(
        !pickEntity(world.scene, world.meshes, downAt(6.0f, 5.0f)).has_value());
  }

  TEST("bounds should follow the transform")
  {
    World world;
    Entity box = world.spawn(world.cube, {1, 2, 3});
    box.getComponent<TransformComponent>().scale = {2, 2, 2};

    const auto bounds = entityBounds(box, world.meshes);
    CHECK(testing::approx(bounds->min.x, 0.0f));
    CHECK(testing::approx(bounds->max.z, 5.0f));
  }

  TEST("a child should be picked where its parent puts it")
  {
    World world;
    Entity parent = world.spawn(MeshHandle{}, {10, 0, 0});
    Entity child = world.spawn(world.cube, {0, 3, 0});
    hierarchy::setParent(child, parent);

    const auto hit = pickEntity(world.scene, world.meshes, downAt(10.0f, 3.0f));
    CHECK(hit.has_value());
    CHECK(hit->entity.getId() == child.getId());
    CHECK(
        !pickEntity(world.scene, world.meshes, downAt(0.0f, 3.0f)).has_value());
  }

  TEST("a surface pick should find the part of the mesh under the ray")
  {
    World world;
    // The box's top face (its first 6 indices) as one section, the rest
    // another.
    MeshData twoPart = meshes::box({1, 1, 1});
    twoPart.sections = {
        {0, 6, MeshMaterial{}, "Top"}, {6, 30, MeshMaterial{}, "Sides"}};
    const MeshHandle part = world.meshes.add(twoPart);
    Entity box = world.spawn(part, {0, 0, 0});

    const auto top = pickSurface(world.scene, world.meshes, downAt(0.1f, 0.1f));
    CHECK(top.has_value());
    if (top)
    {
      CHECK(top->entity == box);
      CHECK(top->section == 0u);
      CHECK(testing::approx(top->distance, 49.0f)); // the top is at z = 1
    }

    const auto side =
        pickSurface(world.scene, world.meshes, Ray{{10, 0, 0.5f}, {-1, 0, 0}});
    CHECK(side.has_value());
    if (side)
      CHECK(side->section == 1u);
    CHECK(!pickSurface(world.scene, world.meshes, downAt(3.0f, 3.0f))
               .has_value());
  }

  TEST("a surface pick should follow the object's transform, and skip "
       "bounds-only hits")
  {
    World world;
    Entity moved = world.spawn(world.cube, {5, 0, 0});
    moved.getComponent<TransformComponent>().scale = {2, 2, 2};
    CHECK(pickSurface(world.scene, world.meshes, downAt(5.9f, 0.0f))
              .has_value()); // scaled to +-1
    CHECK(!pickSurface(world.scene, world.meshes, downAt(0.0f, 0.0f))
               .has_value());

    // A tilted plane's bounds cover a corner its triangles don't reach.
    MeshData plane;
    plane.vertices = {{{0, 0, 0}}, {{1, 0, 0}}, {{0, 1, 1}}};
    plane.indices = {0, 1, 2};
    world.spawn(world.meshes.add(plane), {10, 10, 0});
    CHECK(pickEntity(world.scene, world.meshes, downAt(10.9f, 10.9f))
              .has_value());
    CHECK(!pickSurface(world.scene, world.meshes, downAt(10.9f, 10.9f))
               .has_value());
  }

  return testing::report("pickingTests");
}
