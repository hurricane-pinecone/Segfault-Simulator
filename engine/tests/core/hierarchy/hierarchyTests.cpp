#include "../../testHarness.h"

#include <engine/core/components/parentComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/ecs/ecs.h>
#include <engine/core/hierarchy/hierarchy.h>

#include <glm/glm/gtc/quaternion.hpp>

#include <vector>

using namespace sfs;

namespace
{
constexpr float kHalfTurn = 1.5707964f; // 90 degrees

Entity placed(Registry& registry, glm::vec3 position)
{
  Entity entity = registry.createEntity();
  entity.addComponent<TransformComponent>(position);
  return entity;
}

bool approxVec(const glm::vec3& a, const glm::vec3& b)
{
  return testing::approx(a.x, b.x) && testing::approx(a.y, b.y) &&
         testing::approx(a.z, b.z);
}
} // namespace

int main()
{
  TEST("a child's world matrix should be its parent's times its own")
  {
    Registry registry;
    Entity parent = placed(registry, {10.0f, 0.0f, 0.0f});
    parent.getComponent<TransformComponent>().setYaw(kHalfTurn);
    parent.getComponent<TransformComponent>().scale = glm::vec3(2.0f);
    Entity child = placed(registry, {1.0f, 0.0f, 0.0f});
    CHECK(hierarchy::setParent(child, parent));

    // +X turned a quarter about Z is +Y, doubled by the parent's scale.
    CHECK(approxVec(hierarchy::worldPosition(child), {10.0f, 2.0f, 0.0f}));
    CHECK(hierarchy::parentOf(child) == parent);
    CHECK(!hierarchy::parentOf(parent).isValid());
  }

  TEST("an ancestor without a transform should only group")
  {
    Registry registry;
    Entity group = registry.createEntity();
    Entity child = placed(registry, {1.0f, 2.0f, 3.0f});
    hierarchy::setParent(child, group);
    CHECK(approxVec(hierarchy::worldPosition(child), {1.0f, 2.0f, 3.0f}));
  }

  TEST("setParent should refuse loops and leave the hierarchy unchanged")
  {
    Registry registry;
    Entity a = placed(registry, {});
    Entity b = placed(registry, {});
    Entity c = placed(registry, {});
    hierarchy::setParent(b, a);
    hierarchy::setParent(c, b);

    CHECK(!hierarchy::setParent(a, c)); // a would be its own grandchild
    CHECK(!hierarchy::setParent(a, a));
    CHECK(!hierarchy::parentOf(a).isValid());
    CHECK(hierarchy::isAncestor(a, c));
    CHECK(!hierarchy::isAncestor(c, a));
  }

  TEST("making an entity a root should remove its ParentComponent")
  {
    Registry registry;
    Entity parent = placed(registry, {});
    Entity child = placed(registry, {});
    hierarchy::setParent(child, parent);
    CHECK(hierarchy::setParent(child, {}));
    CHECK(!child.hasComponent<ParentComponent>());
  }

  TEST("setParentKeepWorld should leave the child where it was in the world")
  {
    Registry registry;
    Entity parent = placed(registry, {5.0f, 5.0f, 0.0f});
    parent.getComponent<TransformComponent>().setYaw(kHalfTurn);
    Entity child = placed(registry, {6.0f, 5.0f, 1.0f});
    child.getComponent<TransformComponent>().scale = glm::vec3(3.0f);

    CHECK(hierarchy::setParentKeepWorld(child, parent));
    CHECK(approxVec(hierarchy::worldPosition(child), {6.0f, 5.0f, 1.0f}));
    // Local now: 1 unit along the parent's turned axis (world +X is its -Y).
    CHECK(approxVec(child.getComponent<TransformComponent>().position,
                    {0.0f, -1.0f, 1.0f}));
    CHECK(approxVec(
        child.getComponent<TransformComponent>().scale, glm::vec3(3.0f)));

    CHECK(hierarchy::setParentKeepWorld(child, {}));
    CHECK(approxVec(
        child.getComponent<TransformComponent>().position, {6.0f, 5.0f, 1.0f}));
  }

  TEST("setWorldMatrix should place a child at a world position")
  {
    Registry registry;
    Entity parent = placed(registry, {0.0f, 0.0f, 4.0f});
    Entity child = placed(registry, {});
    hierarchy::setParent(child, parent);
    hierarchy::setWorldMatrix(
        child, TransformComponent({1.0f, 1.0f, 1.0f}).matrix());
    CHECK(approxVec(child.getComponent<TransformComponent>().position,
                    {1.0f, 1.0f, -3.0f}));
  }

  TEST("a destroyed parent should leave its children as roots")
  {
    Registry registry;
    Entity parent = placed(registry, {9.0f, 0.0f, 0.0f});
    Entity child = placed(registry, {1.0f, 0.0f, 0.0f});
    hierarchy::setParent(child, parent);
    registry.flushEntities();
    registry.destroyEntity(parent);
    registry.flushEntities();
    // The id is reused, but the generation tells the new entity apart.
    Entity newcomer = placed(registry, {50.0f, 0.0f, 0.0f});
    registry.flushEntities();
    CHECK(newcomer.getId() == parent.getId());

    CHECK(!hierarchy::parentOf(child).isValid());
    CHECK(approxVec(hierarchy::worldPosition(child), {1.0f, 0.0f, 0.0f}));
  }

  TEST("EntityTree should walk parents before children, siblings in list order")
  {
    Registry registry;
    Entity a = placed(registry, {});
    Entity b = placed(registry, {});
    Entity a1 = placed(registry, {});
    Entity b1 = placed(registry, {});
    Entity a2 = placed(registry, {});
    Entity a1x = placed(registry, {});
    hierarchy::setParent(a1, a);
    hierarchy::setParent(a2, a);
    hierarchy::setParent(b1, b);
    hierarchy::setParent(a1x, a1);
    registry.flushEntities();

    const hierarchy::EntityTree tree(registry.entities());
    CHECK(tree.roots() == std::vector<Entity>{a, b});
    CHECK(tree.children(a) == std::vector<Entity>{a1, a2});
    CHECK(tree.preOrder() == std::vector<Entity>{a, a1, a1x, a2, b, b1});
    CHECK(tree.subtree(a1) == std::vector<Entity>{a1, a1x});
    CHECK(tree.children(b1).empty());
  }

  TEST("EntityTree should still reach entities in a loop written directly")
  {
    Registry registry;
    Entity a = placed(registry, {});
    Entity b = placed(registry, {});
    a.addComponent<ParentComponent>(b);
    b.addComponent<ParentComponent>(a);
    registry.flushEntities();

    const hierarchy::EntityTree tree(registry.entities());
    CHECK(tree.preOrder().size() == 2u);
  }

  return testing::report("hierarchyTests");
}
