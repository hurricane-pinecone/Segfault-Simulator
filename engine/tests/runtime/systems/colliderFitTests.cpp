#include "../../testHarness.h"
#include "../rendering/mockRenderer.h"

#include <engine/core/components/meshRendererComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/hierarchy/hierarchy.h>
#include <engine/core/rendering/meshPrimitives.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/rendering/meshLibrary.h>
#include <engine/runtime/sceneManager/scene.h>
#include <engine/runtime/systems/colliderFit.h>

#include <glm/glm/ext/quaternion_trigonometric.hpp>

using namespace sfs;
using sfs::testing3d::MockRenderer;

namespace
{
bool near(const glm::vec3& a, const glm::vec3& b)
{
  return testing::approx(a.x, b.x) && testing::approx(a.y, b.y) &&
         testing::approx(a.z, b.z);
}

Aabb box(const glm::vec3& min, const glm::vec3& max)
{
  Aabb bounds;
  bounds.expand(min);
  bounds.expand(max);
  return bounds;
}

// A box mesh of `size` centred on `centre` (in its own space; meshes::box
// stands on z = 0).
MeshData boxAt(const glm::vec3& size, const glm::vec3& centre)
{
  MeshData mesh = meshes::box(size);
  for (MeshVertex& vertex : mesh.vertices)
    vertex.position += centre - glm::vec3(0.0f, 0.0f, size.z * 0.5f);
  return mesh;
}

struct World
{
  AssetStore store;
  MockRenderer renderer;
  Scene scene{0, SceneServices{.assetStore = store, .renderer = &renderer}};
  MeshLibrary meshes{renderer};

  Entity spawn(const MeshData* mesh,
               const TransformComponent& transform = TransformComponent{},
               const Entity& parent = {})
  {
    Entity entity = scene.createEntity();
    entity.addComponent<TransformComponent>(transform);
    if (mesh)
      entity.addComponent<MeshRendererComponent>(
          MeshRendererComponent{meshes.add(*mesh)});
    if (parent.isValid())
      hierarchy::setParent(entity, parent);
    return entity;
  }
};
} // namespace

int main()
{
  TEST("a box collider should take a box's size and centre")
  {
    BoxCollider3DComponent collider;
    collider.trigger = true;
    fitCollider(collider, box({-1, 2, 0}, {3, 4, 1}));
    CHECK(near(collider.size, {4, 2, 1}));
    CHECK(near(collider.offset, {1, 3, 0.5f}));
    CHECK(collider.trigger); // the rest is left alone
  }

  TEST("a flat box should become a slab under its surface")
  {
    BoxCollider3DComponent collider;
    fitCollider(collider, box({-5, -5, 0}, {5, 5, 0}));
    CHECK(near(collider.size, {10, 10, kMinColliderThickness}));
    CHECK(testing::approx(collider.offset.z, -kMinColliderThickness * 0.5f));
  }

  TEST("an empty box should change nothing")
  {
    BoxCollider3DComponent collider;
    collider.size = {2, 3, 4};
    fitCollider(collider, Aabb{});
    CHECK(near(collider.size, {2, 3, 4}));
  }

  TEST("a sphere should take the largest half-size, a capsule the box's "
       "height and its widest flat half-size")
  {
    const Aabb bounds = box({-0.5f, -1.0f, 0.0f}, {0.5f, 1.0f, 3.0f});
    SphereCollider3DComponent sphere;
    fitCollider(sphere, bounds);
    CHECK(testing::approx(sphere.radius, 1.5f));
    CHECK(near(sphere.offset, {0, 0, 1.5f}));
    CapsuleCollider3DComponent capsule;
    fitCollider(capsule, bounds);
    CHECK(testing::approx(capsule.radius, 1.0f));
    CHECK(testing::approx(capsule.height, 3.0f));
    CHECK(near(capsule.offset, {0, 0, 1.5f}));
    // Squat: never shorter than its two ends.
    fitCollider(capsule, box({-1, -1, 0}, {1, 1, 0.5f}));
    CHECK(testing::approx(capsule.height, 2.0f));
  }

  TEST("an entity with a mesh should fit its own mesh, not its children's, "
       "unless asked")
  {
    World world;
    const MeshData body = boxAt({2, 1, 1}, {0, 0, 0.5f});
    const MeshData arm = boxAt({1, 1, 1}, {0, 0, 0});
    Entity root = world.spawn(&body);
    world.spawn(&arm, TransformComponent{glm::vec3(3, 0, 0.5f)}, root);
    root.addComponent<BoxCollider3DComponent>();
    CHECK(fitColliderToMesh<BoxCollider3DComponent>(root, world.meshes));
    const auto& collider = root.getComponent<BoxCollider3DComponent>();
    CHECK(near(collider.size, {2, 1, 1}));
    CHECK(near(collider.offset, {0, 0, 0.5f}));

    CHECK(fitColliderToMesh<BoxCollider3DComponent>(root, world.meshes, true));
    CHECK(near(collider.size, {4.5f, 1, 1})); // x from -1 to 3.5
    CHECK(near(collider.offset, {1.25f, 0, 0.5f}));
  }

  TEST("an entity with no mesh should fit its children's, in its own space")
  {
    World world;
    const MeshData cube = boxAt({1, 1, 1}, {0, 0, 0});
    // A group turned a quarter about Z and scaled 2: its children's bounds
    // come back into its axes and units.
    Entity group =
        world.spawn(nullptr,
                    TransformComponent{
                        glm::vec3(10, 0, 0),
                        glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1)),
                        glm::vec3(2.0f)});
    world.spawn(&cube, TransformComponent{glm::vec3(-1, 0, 0)}, group);
    world.spawn(&cube, TransformComponent{glm::vec3(1, 0, 0)}, group);
    group.addComponent<SphereCollider3DComponent>();
    const Aabb bounds = entityMeshBounds(group, world.meshes);
    CHECK(near(bounds.min, {-1.5f, -0.5f, -0.5f}));
    CHECK(near(bounds.max, {1.5f, 0.5f, 0.5f}));
    CHECK(fitColliderToMesh<SphereCollider3DComponent>(group, world.meshes));
    CHECK(testing::approx(
        group.getComponent<SphereCollider3DComponent>().radius, 1.5f));
  }

  TEST("no collider of that kind, or no meshes, should change nothing")
  {
    World world;
    const MeshData cube = boxAt({1, 1, 1}, {0, 0, 0});
    Entity meshed = world.spawn(&cube);
    Entity empty = world.spawn(nullptr);
    empty.addComponent<CapsuleCollider3DComponent>();
    CHECK(!fitColliderToMesh<BoxCollider3DComponent>(meshed, world.meshes));
    CHECK(!fitColliderToMesh<CapsuleCollider3DComponent>(empty, world.meshes));
    CHECK(testing::approx(
        empty.getComponent<CapsuleCollider3DComponent>().height, 1.8f));
  }

  return testing::report("colliderFitTests");
}
