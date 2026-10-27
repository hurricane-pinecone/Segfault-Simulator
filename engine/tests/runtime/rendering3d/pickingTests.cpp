#include "../../testHarness.h"
#include "mockRenderer3D.h"

#include <engine/core/components/meshRendererComponent.h>
#include <engine/core/components/transform3DComponent.h>
#include <engine/core/rendering/3d/meshPrimitives.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/rendering3d/meshLibrary.h>
#include <engine/runtime/rendering3d/picking.h>
#include <engine/runtime/sceneManager/scene.h>

using namespace sfs;
using sfs::testing3d::MockRenderer3D;

namespace
{
struct World
{
  AssetStore store;
  MockRenderer3D renderer;
  Scene scene{0, SceneServices{.assetStore = store, .renderer3D = &renderer}};
  MeshLibrary meshes{renderer};
  MeshHandle cube = meshes.add(meshes::box({1, 1, 1}));
  MeshHandle floor = meshes.add(meshes::box({20, 20, 0.1f}));

  Entity spawn(MeshHandle mesh, const glm::vec3& position)
  {
    Entity entity = scene.createEntity();
    entity.addComponent<Transform3DComponent>(Transform3DComponent{position});
    if (mesh.valid())
      entity.addComponent<MeshRendererComponent>(MeshRendererComponent{mesh});
    return entity;
  }
};

// Looking straight down at (x, y).
Ray3D downAt(float x, float y) { return Ray3D{{x, y, 50.0f}, {0.0f, 0.0f, -1.0f}}; }
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
    CHECK(!pickEntity(world.scene, world.meshes, downAt(30.0f, 30.0f)).has_value());
  }

  TEST("inside several boxes, the smallest should win")
  {
    World world;
    world.spawn(world.floor, {0, 0, -0.05f});
    Entity box = world.spawn(world.cube, {0, 0, -0.5f});

    // Starts inside both the floor and the box.
    const Ray3D ray{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}};
    const auto hit = pickEntity(world.scene, world.meshes, ray);
    CHECK(hit->entity.getId() == box.getId());
  }

  TEST("entities without a mesh should be pickable by a marker")
  {
    World world;
    Entity light = world.spawn(MeshHandle{}, {5, 5, 2});

    const auto hit = pickEntity(world.scene, world.meshes, downAt(5.1f, 5.0f));
    CHECK(hit->entity.getId() == light.getId());
    CHECK(!pickEntity(world.scene, world.meshes, downAt(6.0f, 5.0f)).has_value());
  }

  TEST("bounds should follow the transform")
  {
    World world;
    Entity box = world.spawn(world.cube, {1, 2, 3});
    box.getComponent<Transform3DComponent>().scale = {2, 2, 2};

    const auto bounds = entityBounds(box, world.meshes);
    CHECK(testing::approx(bounds->min.x, 0.0f));
    CHECK(testing::approx(bounds->max.z, 5.0f));
  }

  return testing::report("pickingTests");
}
