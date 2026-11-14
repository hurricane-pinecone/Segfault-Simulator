#include "../../testHarness.h"

#include <engine/core/components/nameComponent.h>
#include <engine/core/components/restPoseComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/ecs/ecs.h>
#include <engine/core/hierarchy/hierarchy.h>

#include <glm/glm/gtc/quaternion.hpp>

#include <cstdint>
#include <random>
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

struct Tagged
{
};

std::vector<Entity> children(const Entity& entity)
{
  std::vector<Entity> out;
  for (const Entity& child : hierarchy::childrenOf(entity))
    out.push_back(child);
  return out;
}

std::vector<Entity> below(const Entity& root)
{
  std::vector<Entity> out;
  for (const Entity& entity : hierarchy::descendants(root))
    out.push_back(entity);
  return out;
}

// The preorder worked out from parentOf alone, recursively, as a reference.
void appendReference(Registry& registry,
                     const std::vector<Entity>& all,
                     const Entity& entity,
                     uint32_t parentPos,
                     std::vector<HierarchyLinks::Placed>& out)
{
  const uint32_t pos = static_cast<uint32_t>(out.size());
  out.push_back({entity.getId(), parentPos});
  for (const Entity& other : all)
    if (hierarchy::parentOf(other) == entity)
      appendReference(registry, all, other, pos, out);
}

bool samePlaced(const std::vector<HierarchyLinks::Placed>& a,
                const std::vector<HierarchyLinks::Placed>& b)
{
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (a[i].id != b[i].id || a[i].parentPos != b[i].parentPos)
      return false;
  return true;
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

  TEST("making an entity a root should take it out of its parent's children")
  {
    Registry registry;
    Entity parent = placed(registry, {});
    Entity child = placed(registry, {});
    hierarchy::setParent(child, parent);
    CHECK(hierarchy::setParent(child, {}));
    CHECK(!hierarchy::parentOf(child).isValid());
    CHECK(!hierarchy::hasChildren(parent));
    CHECK(hierarchy::childrenOf(parent).empty());
  }

  TEST("setParent should update both sides at once, before any flush")
  {
    Registry registry;
    Entity parent = placed(registry, {});
    Entity child = placed(registry, {});
    CHECK(hierarchy::setParent(child, parent));
    CHECK(hierarchy::parentOf(child) == parent);
    CHECK(children(parent) == std::vector<Entity>{child});
    CHECK(hierarchy::hasChildren(parent));
    CHECK(!hierarchy::hasChildren(child));
  }

  TEST("siblings should be in id order, whatever order they were added in")
  {
    Registry registry;
    Entity parent = registry.createEntity();
    std::vector<Entity> made;
    for (int i = 0; i < 6; ++i)
      made.push_back(registry.createEntity());
    // Out of order: the middle, the ends, then the rest.
    for (int i : {3, 0, 5, 1, 4, 2})
      hierarchy::setParent(made[static_cast<std::size_t>(i)], parent);
    CHECK(children(parent) == made);

    // A freed low id, reused, goes back in its place.
    registry.destroyEntity(made[1]);
    registry.flushEntities();
    CHECK((children(parent) ==
           std::vector<Entity>{made[0], made[2], made[3], made[4], made[5]}));
    Entity reused = registry.createEntity();
    CHECK(reused.getId() == made[1].getId());
    CHECK(!hierarchy::parentOf(reused).isValid()); // a new id starts unlinked
    CHECK(!hierarchy::hasChildren(reused));
    hierarchy::setParent(reused, parent);
    CHECK((children(parent) ==
           std::vector<Entity>{
               made[0], reused, made[2], made[3], made[4], made[5]}));
  }

  TEST("reparenting should move a child from one parent's list to another's")
  {
    Registry registry;
    Entity a = registry.createEntity();
    Entity b = registry.createEntity();
    Entity x = registry.createEntity();
    Entity y = registry.createEntity();
    Entity z = registry.createEntity();
    hierarchy::setParent(x, a);
    hierarchy::setParent(y, a);
    hierarchy::setParent(z, a);
    hierarchy::setParent(y, b); // the middle one
    CHECK((children(a) == std::vector<Entity>{x, z}));
    CHECK(children(b) == std::vector<Entity>{y});
    hierarchy::setParent(z, b); // the last one
    hierarchy::setParent(x, b); // the first one, now the only one
    CHECK(children(a).empty());
    CHECK((children(b) == std::vector<Entity>{x, y, z}));
    CHECK(hierarchy::parentOf(x) == b);
  }

  TEST("destroying should unlink at the flush, its children becoming roots")
  {
    Registry registry;
    Entity top = registry.createEntity();
    Entity middle = registry.createEntity();
    Entity left = registry.createEntity();
    Entity right = registry.createEntity();
    hierarchy::setParent(middle, top);
    hierarchy::setParent(left, middle);
    hierarchy::setParent(right, middle);
    registry.flushEntities();

    registry.destroyEntity(middle);
    // Still alive until the flush, and still linked.
    CHECK(hierarchy::parentOf(left) == middle);
    CHECK(children(top) == std::vector<Entity>{middle});
    registry.flushEntities();
    CHECK(children(top).empty());
    CHECK(!hierarchy::parentOf(left).isValid());
    CHECK(!hierarchy::parentOf(right).isValid());
    // The roots now, in id order.
    std::vector<Entity::EntityId> roots;
    for (const HierarchyLinks::Placed& placed : hierarchy::preorder(registry))
      if (placed.parentPos == HierarchyLinks::kNone)
        roots.push_back(placed.id);
    CHECK((roots == std::vector<Entity::EntityId>{
                        top.getId(), left.getId(), right.getId()}));
  }

  TEST("a destroyed or stale handle should have no parent and no children")
  {
    Registry registry;
    Entity parent = registry.createEntity();
    Entity child = registry.createEntity();
    hierarchy::setParent(child, parent);
    registry.destroyEntity(parent);
    registry.destroyEntity(child);
    registry.flushEntities();
    Entity newParent = registry.createEntity(); // the old ids again
    Entity newChild = registry.createEntity();
    hierarchy::setParent(newChild, newParent);

    CHECK(!hierarchy::parentOf(child).isValid());
    CHECK(hierarchy::childrenOf(parent).empty());
    CHECK(!hierarchy::hasChildren(parent));
    CHECK(below(parent).empty());
    CHECK(!hierarchy::isAncestor(parent, newChild));
    CHECK(hierarchy::childrenOf(Entity()).empty());
    CHECK(!hierarchy::parentOf(Entity()).isValid());
    // Putting a destroyed entity under a parent is refused (logged).
    CHECK(!hierarchy::setParent(child, newParent));
    CHECK(children(newParent) == std::vector<Entity>{newChild});
  }

  TEST("findChild and findChildren should pick children by a tag")
  {
    Registry registry;
    Entity parent = registry.createEntity();
    Entity plain = registry.createEntity();
    Entity first = registry.createEntity();
    Entity second = registry.createEntity();
    Entity grandchild = registry.createEntity();
    for (Entity child : {plain, first, second})
      hierarchy::setParent(child, parent);
    hierarchy::setParent(grandchild, plain);
    first.addComponent<Tagged>();
    second.addComponent<Tagged>();
    grandchild.addComponent<Tagged>();

    CHECK(hierarchy::findChild<Tagged>(parent) == first);
    CHECK(!hierarchy::findChild<Tagged>(first).isValid());
    std::vector<Entity> found{plain}; // cleared first
    hierarchy::findChildren<Tagged>(parent, found);
    CHECK((found == std::vector<Entity>{first, second}));
    CHECK(hierarchy::findDescendant<Tagged>(parent) == grandchild);
    CHECK(!hierarchy::findDescendant<NameComponent>(parent).isValid());
  }

  TEST("descendants should walk in preorder, skipping subtrees when asked")
  {
    Registry registry;
    Entity root = registry.createEntity();
    Entity a = registry.createEntity();
    Entity a1 = registry.createEntity();
    Entity a1x = registry.createEntity();
    Entity a2 = registry.createEntity();
    Entity b = registry.createEntity();
    Entity b1 = registry.createEntity();
    Entity outside = registry.createEntity();
    hierarchy::setParent(a, root);
    hierarchy::setParent(b, root);
    hierarchy::setParent(a1, a);
    hierarchy::setParent(a2, a);
    hierarchy::setParent(a1x, a1);
    hierarchy::setParent(b1, b);
    (void)outside;

    CHECK((below(root) == std::vector<Entity>{a, a1, a1x, a2, b, b1}));
    CHECK((below(a) == std::vector<Entity>{a1, a1x, a2}));
    CHECK(below(a1x).empty()); // a leaf has none

    std::vector<Entity> walked;
    const hierarchy::DescendantRange range = hierarchy::descendants(root);
    for (auto it = range.begin(); it != range.end(); ++it)
    {
      walked.push_back(*it);
      if (*it == a1 || *it == b)
        it.skipChildren();
    }
    CHECK((walked == std::vector<Entity>{a, a1, a2, b}));
  }

  TEST("preorder should match one built from parentOf, on a random tree")
  {
    Registry registry;
    std::mt19937 random(7);
    std::vector<Entity> all;
    for (int i = 0; i < 2000; ++i)
    {
      Entity entity = registry.createEntity();
      if (!all.empty() && random() % 5 != 0)
        hierarchy::setParent(entity, all[random() % all.size()]);
      all.push_back(entity);
    }
    // Some moved about, and some destroyed (their children become roots).
    for (int i = 0; i < 300; ++i)
      hierarchy::setParent(
          all[random() % all.size()], all[random() % all.size()]);
    for (int i = 0; i < 100; ++i)
      registry.destroyEntity(all[random() % all.size()]);
    registry.flushEntities();

    std::vector<Entity> living;
    for (const Entity& entity : all)
      if (entity.isAlive())
        living.push_back(entity);
    std::vector<HierarchyLinks::Placed> reference;
    for (const Entity& entity : living)
      if (!hierarchy::parentOf(entity).isValid())
        appendReference(
            registry, living, entity, HierarchyLinks::kNone, reference);
    CHECK(samePlaced(hierarchy::preorder(registry), reference));
    // Asked again with nothing changed: the same list, not rebuilt.
    const std::vector<HierarchyLinks::Placed>* first =
        &hierarchy::preorder(registry);
    const uint64_t builtAt = registry.links().preorderBuiltAt;
    CHECK(&hierarchy::preorder(registry) == first);
    CHECK(registry.links().preorderBuiltAt == builtAt);
    // A new entity (a freed id, reused) is listed at once, as a root.
    Entity late = registry.createEntity();
    bool listed = false;
    for (const HierarchyLinks::Placed& placed : hierarchy::preorder(registry))
      listed = listed || (placed.id == late.getId() &&
                          placed.parentPos == HierarchyLinks::kNone);
    CHECK(listed);
    CHECK(hierarchy::preorder(registry).size() == reference.size() + 1);
  }

  TEST("preorder should drop a removed top-level leaf")
  {
    Registry registry;
    Entity first = registry.createEntity();
    Entity leaf = registry.createEntity();
    Entity last = registry.createEntity();
    registry.flushEntities();
    CHECK(hierarchy::preorder(registry).size() == 3u); // built before
    registry.destroyEntity(leaf); // no parent, no children: no link moves
    registry.flushEntities();
    const std::vector<HierarchyLinks::Placed>& order =
        hierarchy::preorder(registry);
    CHECK(order.size() == 2u);
    CHECK(order.size() == 2u && order[0].id == first.getId() &&
          order[1].id == last.getId());
  }

  TEST("roots should list the entities with no parent, in id order")
  {
    Registry registry;
    Entity a = registry.createEntity();
    Entity child = registry.createEntity();
    Entity gone = registry.createEntity();
    Entity b = registry.createEntity();
    hierarchy::setParent(child, a);
    registry.destroyEntity(gone);
    registry.flushEntities();
    std::vector<Entity> roots;
    for (const Entity& root : hierarchy::roots(registry))
      roots.push_back(root);
    CHECK((roots == std::vector<Entity>{a, b}));
    Registry empty;
    CHECK(hierarchy::roots(empty).begin() == hierarchy::roots(empty).end());
  }

  TEST("a walk should end, not crash, when its entity is moved out mid-walk")
  {
    Registry registry;
    Entity root = registry.createEntity();
    Entity elsewhere = registry.createEntity();
    Entity cargo = registry.createEntity();
    Entity part = registry.createEntity();
    hierarchy::setParent(cargo, root);
    hierarchy::setParent(part, cargo);

    // Unloaded while it's the current one: made a root.
    int steps = 0;
    for (const Entity& entity : hierarchy::descendants(root))
    {
      ++steps;
      if (entity == part)
        hierarchy::setParent(part, {});
      CHECK(steps < 10);
      if (steps >= 10)
        break;
    }
    CHECK(steps == 2);

    // Moved under another root while it's the current one.
    hierarchy::setParent(part, cargo);
    steps = 0;
    for (const Entity& entity : hierarchy::descendants(root))
    {
      ++steps;
      if (entity == part)
        hierarchy::setParent(part, elsewhere);
      if (steps >= 10)
        break;
    }
    CHECK(steps == 2);

    // Destroyed and flushed while it's the current one.
    hierarchy::setParent(part, cargo);
    steps = 0;
    for (const Entity& entity : hierarchy::descendants(root))
    {
      ++steps;
      if (entity == part)
      {
        registry.destroyEntity(part);
        registry.flushEntities();
      }
      if (steps >= 10)
        break;
    }
    CHECK(steps == 2);
  }

  TEST("canParent should accept exactly what setParent does")
  {
    Registry registry;
    Registry other;
    Entity parent = registry.createEntity();
    Entity child = registry.createEntity();
    Entity stranger = other.createEntity();
    CHECK(hierarchy::canParent(child, parent));
    CHECK(!hierarchy::canParent(stranger, parent)); // another registry
    CHECK(!hierarchy::setParent(stranger, parent));
    registry.destroyEntity(child);
    registry.flushEntities();
    CHECK(!hierarchy::canParent(child, parent)); // destroyed
    CHECK(!hierarchy::setParent(child, parent));
    CHECK(hierarchy::canParent(child, {})); // a root: a no-op
    CHECK(hierarchy::setParent(child, {}));
  }

  TEST("linkVersion should move only when a link changes")
  {
    Registry registry;
    Entity parent = registry.createEntity();
    Entity child = registry.createEntity();
    registry.flushEntities();
    uint64_t version = registry.links().linkVersion;

    registry.createEntity(); // a new root
    CHECK(registry.links().linkVersion == version);
    hierarchy::setParent(child, parent);
    CHECK(registry.links().linkVersion > version);
    version = registry.links().linkVersion;
    hierarchy::setParent(child, parent); // where it already is
    CHECK(registry.links().linkVersion == version);
    Entity loner = registry.createEntity();
    registry.flushEntities();
    version = registry.links().linkVersion;
    registry.destroyEntity(loner); // a root with no children
    registry.flushEntities();
    CHECK(registry.links().linkVersion == version);
    registry.destroyEntity(parent); // a parent
    registry.flushEntities();
    CHECK(registry.links().linkVersion > version);
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

  TEST("worldTransform should be a root's own, and a child's in the world")
  {
    Registry reg;
    Entity parent = reg.createEntity();
    parent.addComponent<TransformComponent>(TransformComponent{
        glm::vec2(10.0f, 0.0f), glm::vec2(-2.0f, 2.0f), glm::radians(90.0f)});
    Entity child = reg.createEntity();
    child.addComponent<TransformComponent>(
        TransformComponent{glm::vec3(3.0f, 0.0f, 1.0f)});
    hierarchy::setParent(child, parent);

    // A root's comes back as it is, mirrored scale and all.
    const TransformComponent root = hierarchy::worldTransform(parent);
    CHECK(testing::approx(root.scale.x, -2.0f));

    // The child: 3 along the parent's mirrored, turned x is 6 down -y.
    const TransformComponent world = hierarchy::worldTransform(child);
    CHECK(testing::approx(world.position2D().x, 10.0f));
    CHECK(testing::approx(world.position2D().y, -6.0f));
    CHECK(testing::approx(world.position.z, 1.0f)); // its height kept
    CHECK(glm::abs(world.scale2D().x) > 1.99f);
    // It's mirrored, as its parent is: its matrix is the same.
    const glm::mat4 a = world.matrix();
    const glm::mat4 b = hierarchy::worldMatrix(child);
    for (int column = 0; column < 4; ++column)
      for (int row = 0; row < 3; ++row)
        CHECK(testing::approx(a[column][row], b[column][row]));
  }

  TEST("setWorldTransform should move a child to a world placement")
  {
    Registry reg;
    Entity parent = reg.createEntity();
    parent.addComponent<TransformComponent>(
        TransformComponent{glm::vec2(5.0f, 5.0f)});
    Entity child = reg.createEntity();
    child.addComponent<TransformComponent>();
    hierarchy::setParent(child, parent);

    TransformComponent world;
    world.setPosition2D({7.0f, 1.0f});
    world.scale = glm::vec3(-1.0f, 1.0f, 1.0f);
    hierarchy::setWorldTransform(child, world);

    const auto& local = child.getComponent<TransformComponent>();
    CHECK(testing::approx(local.position.x, 2.0f));
    CHECK(testing::approx(local.position.y, -4.0f));
    CHECK(testing::approx(local.scale.x, -1.0f)); // still mirrored
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

  TEST("preorder should list parents before children, siblings in id order")
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

    std::vector<Entity::EntityId> ids;
    std::vector<uint32_t> parents;
    for (const HierarchyLinks::Placed& placed : hierarchy::preorder(registry))
    {
      ids.push_back(placed.id);
      parents.push_back(placed.parentPos);
    }
    constexpr uint32_t kRoot = HierarchyLinks::kNone;
    CHECK((ids == std::vector<Entity::EntityId>{a.getId(),
                                                a1.getId(),
                                                a1x.getId(),
                                                a2.getId(),
                                                b.getId(),
                                                b1.getId()}));
    CHECK((parents == std::vector<uint32_t>{kRoot, 0, 1, 0, kRoot, 4}));
  }

  TEST("recordRestPose should add or refresh the rest pose, given a transform")
  {
    Registry registry;
    Entity part = placed(registry, {1.0f, 2.0f, 3.0f});
    TransformComponent& transform = part.getComponent<TransformComponent>();
    transform.setYaw(kHalfTurn);
    transform.scale = glm::vec3(2.0f);
    hierarchy::recordRestPose(part);
    CHECK(part.hasComponent<RestPoseComponent>());
    const RestPoseComponent& rest = part.getComponent<RestPoseComponent>();
    CHECK(approxVec(rest.position, {1.0f, 2.0f, 3.0f}));
    const glm::quat turned = part.getComponent<TransformComponent>().rotation;
    CHECK(testing::approx(glm::dot(rest.rotation, turned), 1.0f));
    CHECK(approxVec(rest.scale, glm::vec3(2.0f)));

    part.getComponent<TransformComponent>().position = {5.0f, 0.0f, 0.0f};
    CHECK(approxVec(
        part.getComponent<RestPoseComponent>().position, {1.0f, 2.0f, 3.0f}));
    hierarchy::recordRestPose(part);
    CHECK(approxVec(
        part.getComponent<RestPoseComponent>().position, {5.0f, 0.0f, 0.0f}));

    Entity bare = registry.createEntity();
    hierarchy::recordRestPose(bare);
    CHECK(!bare.hasComponent<RestPoseComponent>());
  }

  return testing::report("hierarchyTests");
}
