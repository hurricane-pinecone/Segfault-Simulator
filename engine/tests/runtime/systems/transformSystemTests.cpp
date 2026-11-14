#include "../../testHarness.h"

#include <engine/core/components/transformComponent.h>
#include <engine/core/components/worldTransformComponent.h>
#include <engine/core/ecs/ecs.h>
#include <engine/core/hierarchy/hierarchy.h>
#include <engine/core/logger/logger.h>
#include <engine/core/util/debugFeatures.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/sceneManager/scene.h>
#include <engine/runtime/systems/transformSystem.h>

#include <glm/glm/gtc/quaternion.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <typeinfo>
#include <vector>

using namespace sfs;

namespace
{

struct World
{
  AssetStore store;
  Scene scene{0, SceneServices{.assetStore = store}};

  // An entity with a transform somewhere awkward (rounding in every bit of
  // its world matrix), under `parent` if one is given.
  Entity placed(const Entity& parent = {}, float at = 1.0f)
  {
    Entity entity = scene.createEntity();
    entity.addComponent<TransformComponent>(
        glm::vec3(at, 0.3f * at, -0.7f),
        glm::normalize(glm::quat(0.9f, 0.1f * at, -0.2f, 0.3f)),
        glm::vec3(1.1f, 0.9f, 1.3f));
    if (parent.isValid())
      hierarchy::setParent(entity, parent);
    return entity;
  }

  // A frame: the flush (and update), then the render phase, which opens with
  // the transform pass.
  void frame()
  {
    scene.update(0.0);
    scene.render();
  }
};

const WorldTransformComponent& cacheOf(const Entity& entity)
{
  return *entity.getComponent<WorldTransformComponent>();
}

uint32_t versionOf(const Entity& entity) { return cacheOf(entity).version; }

// Bit-for-bit equality, which == on floats isn't (0 and -0, NaN).
bool same(const glm::mat4& a, const glm::mat4& b)
{
  return std::memcmp(&a, &b, sizeof(a)) == 0;
}

// The entity's world matrix composed from the top with composeWorld(), from
// the transforms alone: the reference every read must equal.
glm::mat4 reference(const Entity& entity)
{
  std::vector<const TransformComponent*> chain;
  for (Entity at = entity; at.isValid(); at = hierarchy::parentOf(at))
    if (TransformComponent* transform = at.getComponent<TransformComponent>())
      chain.push_back(transform);
  if (chain.empty())
    return glm::mat4(1.0f);
  glm::mat4x3 world = hierarchy::composeWorld(nullptr, *chain.back());
  for (auto it = chain.rbegin() + 1; it != chain.rend(); ++it)
    world = hierarchy::composeWorld(&world, **it);
  return glm::mat4(world);
}

// A system whose render() notes whether the entity's cache was made by then.
class CacheProbe : public System
{
public:
  Entity watched;
  bool sawMade = false;

protected:
  void render() override
  {
    sawMade = watched.hasComponent<WorldTransformComponent>() &&
              cacheOf(watched).version != 0;
  }
};

} // namespace

int main()
{
  TEST("the pass should make every cache, equal to worldMatrix bitwise")
  {
    World w;
    Entity root = w.placed({}, 2.0f);
    Entity mid = w.placed(root, 3.0f);
    Entity leaf = w.placed(mid, 5.0f);
    w.frame();
    for (const Entity& entity : {root, mid, leaf})
    {
      CHECK(versionOf(entity) != 0);
      CHECK(same(cacheOf(entity).matrix(), hierarchy::worldMatrix(entity)));
      CHECK(same(hierarchy::cachedWorldMatrix(entity),
                 hierarchy::worldMatrix(entity)));
      CHECK(same(cacheOf(entity).matrix(), reference(entity)));
    }
    CHECK(cacheOf(leaf).parent == mid.getId());
    CHECK(cacheOf(root).parent == HierarchyLinks::kNone);
    CHECK(cacheOf(leaf).position() ==
          glm::vec3(hierarchy::worldMatrix(leaf)[3]));
  }

  TEST("a second pass should change no version when nothing moved")
  {
    World w;
    Entity root = w.placed();
    Entity child = w.placed(root);
    w.frame();
    const uint32_t before = versionOf(child);
    w.frame();
    CHECK(versionOf(root) == before);
    CHECK(versionOf(child) == before);
  }

  TEST("moving a parent should recompose its subtree only")
  {
    World w;
    Entity a = w.placed();
    Entity a1 = w.placed(a);
    Entity a11 = w.placed(a1);
    Entity b = w.placed();
    Entity b1 = w.placed(b);
    w.frame();
    const uint32_t first = versionOf(a);

    a1.getComponent<TransformComponent>()->position.x += 1.0f;
    w.frame();
    CHECK(versionOf(a) == first);
    CHECK(versionOf(b) == first);
    CHECK(versionOf(b1) == first);
    CHECK(versionOf(a1) > first);
    CHECK(versionOf(a11) == versionOf(a1));
    CHECK(cacheOf(a11).matrix() == hierarchy::worldMatrix(a11));
  }

  TEST("an ancestor without a transform should only pass its parent's on")
  {
    World w;
    Entity root = w.placed();
    Entity group = w.scene.createEntity();
    hierarchy::setParent(group, root);
    Entity child = w.placed(group);
    w.frame();
    CHECK(!group.hasComponent<WorldTransformComponent>());
    CHECK(cacheOf(child).parent == root.getId());
    CHECK(
        cacheOf(child).matrix() ==
        glm::mat4(hierarchy::composeWorld(
            &cacheOf(root).world, *child.getComponent<TransformComponent>())));

    // A transform given to the group puts it in the chain.
    group.addComponent<TransformComponent>(glm::vec3(0.0f, 0.0f, 4.0f));
    CHECK(group.hasComponent<WorldTransformComponent>());
    CHECK(hierarchy::worldMatrix(child)[3].z !=
          cacheOf(child).matrix()[3].z); // fresh before the pass
    w.frame();
    CHECK(cacheOf(child).parent == group.getId());
    CHECK(cacheOf(child).matrix() == hierarchy::worldMatrix(child));
  }

  TEST("the cache should come and go with the transform")
  {
    World w;
    Entity entity = w.scene.createEntity();
    w.frame();
    CHECK(!entity.hasComponent<WorldTransformComponent>());
    entity.addComponent<TransformComponent>();
    CHECK(entity.hasComponent<WorldTransformComponent>());
    CHECK(versionOf(entity) == 0);
    w.frame();
    CHECK(versionOf(entity) != 0);
    entity.removeComponent<TransformComponent>();
    CHECK(!entity.hasComponent<WorldTransformComponent>());

    // Made with a transform: cached from its first flush.
    Entity made = w.placed();
    CHECK(!made.hasComponent<WorldTransformComponent>());
    w.frame();
    CHECK(versionOf(made) != 0);
    w.scene.destroyEntity(made);
    w.frame();
    CHECK(!made.isValid());
  }

  TEST("the TransformSystem should be first, its pass before other renders")
  {
    World w;
    CacheProbe& probe = w.scene.addSystem<CacheProbe>();
    probe.watched = w.placed();
    std::vector<std::string> order;
    w.scene.forEachSystem([&](System& system)
                          { order.push_back(typeid(system).name()); });
    CHECK(order.size() == 2u);
    CHECK(order.front() == std::string(typeid(TransformSystem).name()));
    w.frame();
    CHECK(probe.sawMade);
  }

  TEST("worldMatrix should stay fresh after writes at any level")
  {
    World w;
    std::vector<Entity> chain;
    Entity parent;
    for (int level = 0; level < 5; ++level)
      chain.push_back(parent = w.placed(parent, 1.0f + level));
    w.frame();
    // The write, then the read: exactly what a pass would make.
    const auto freshAfterWrite = [&](int level)
    {
      chain[level].getComponent<TransformComponent>()->position.y += 0.25f;
      std::vector<glm::mat4> read;
      for (const Entity& entity : chain)
        read.push_back(hierarchy::worldMatrix(entity));
      w.frame();
      bool same = true;
      for (std::size_t i = 0; i < chain.size(); ++i)
        same = same && ::same(read[i], cacheOf(chain[i]).matrix()) &&
               ::same(read[i], reference(chain[i]));
      return same;
    };
    CHECK(freshAfterWrite(0)); // the root
    CHECK(freshAfterWrite(2)); // the middle
    CHECK(freshAfterWrite(4)); // the leaf

    // A reparent: the leaf's chain is different, with no transform written.
    Entity other = w.placed({}, 9.0f);
    w.frame();
    hierarchy::setParent(chain[4], other);
    const glm::mat4 moved = hierarchy::worldMatrix(chain[4]);
    CHECK(moved == glm::mat4(hierarchy::composeWorld(
                       &cacheOf(other).world,
                       *chain[4].getComponent<TransformComponent>())));
    w.frame();
    CHECK(cacheOf(chain[4]).matrix() == moved);

    // A transform removed from an ancestor: it only groups now.
    chain[1].removeComponent<TransformComponent>();
    const glm::mat4 skipped = hierarchy::worldMatrix(chain[3]);
    w.frame();
    CHECK(cacheOf(chain[3]).parent == chain[2].getId());
    CHECK(cacheOf(chain[2]).parent == chain[0].getId());
    CHECK(cacheOf(chain[3]).matrix() == skipped);
  }

  TEST("an entity made since the pass should be read fresh, and below it too")
  {
    World w;
    Entity root = w.placed();
    Entity child = w.placed(root);
    w.frame();

    // Made after the flush: no cache until the next one. A cached entity
    // moved under it has its cache marked not made by the pass.
    Entity newcomer = w.placed(root, 4.0f);
    hierarchy::setParent(child, newcomer);
    const glm::mat4 expected = hierarchy::worldMatrix(child);
    CHECK(!newcomer.hasComponent<WorldTransformComponent>());
    w.scene.render();
    CHECK(versionOf(child) == 0);
    CHECK(hierarchy::cachedWorldMatrix(child) == expected);
    CHECK(hierarchy::cachedWorldMatrix(newcomer) ==
          hierarchy::worldMatrix(newcomer));
    w.frame();
    CHECK(versionOf(newcomer) != 0);
    CHECK(versionOf(child) != 0);
    CHECK(cacheOf(child).matrix() == expected);
  }

  TEST("a reused id under a cached child should still be read fresh")
  {
    World w;
    Entity holder = w.placed({}, 2.0f);
    Entity child = w.placed(holder, 3.0f);
    w.frame();
    const Entity::EntityId reusedId = holder.getId();
    w.scene.destroyEntity(holder);
    w.scene.update(0.0); // the flush, and no pass since: the child's cache
                         // still names the old holder's id
    CHECK(cacheOf(child).parent == reusedId);
    // A newcomer takes the old holder's id; the child, whose cache was made
    // under that id, goes under it.
    Entity newcomer = w.placed({}, 7.0f);
    CHECK(newcomer.getId() == reusedId);
    hierarchy::setParent(child, newcomer);
    CHECK(same(hierarchy::worldMatrix(child), reference(child)));
    w.scene.render(); // before the newcomer's flush
    CHECK(same(hierarchy::worldMatrix(child), reference(child)));
    CHECK(same(hierarchy::cachedWorldMatrix(child), reference(child)));
    w.scene.update(0.0); // its flush: a cache not made yet
    CHECK(same(hierarchy::worldMatrix(child), reference(child)));
    w.scene.render();
    CHECK(same(cacheOf(child).matrix(), reference(child)));
    CHECK(cacheOf(child).parent == reusedId);
    newcomer.getComponent<TransformComponent>()->position.z += 1.0f;
    CHECK(same(hierarchy::worldMatrix(child), reference(child)));
  }

  TEST("chains deeper than a walk's stack should read fresh with caches")
  {
    World w;
    std::vector<Entity> chain;
    Entity parent;
    for (int level = 0; level < 75; ++level)
      chain.push_back(parent = w.placed(parent, 0.1f * level));
    w.frame();
    for (int level : {0, 5, 31, 32, 33, 42, 63, 64, 74})
    {
      // Clean: straight from the caches.
      CHECK(
          same(hierarchy::worldMatrix(chain.back()), reference(chain.back())));
      chain[level].getComponent<TransformComponent>()->position.x += 0.5f;
      // Written at this level: fresh from there down, in either part.
      CHECK(
          same(hierarchy::worldMatrix(chain.back()), reference(chain.back())));
      CHECK(
          same(hierarchy::worldMatrix(chain[level]), reference(chain[level])));
      w.frame();
      CHECK(same(cacheOf(chain.back()).matrix(), reference(chain.back())));
    }
  }

  TEST("cachedWorldTransform should be worldTransform, read from the cache")
  {
    World w;
    Entity root = w.placed({}, 2.0f);
    Entity child = w.placed(root, 3.0f);
    w.frame();
    const auto equal =
        [](const TransformComponent& a, const TransformComponent& b)
    { return std::memcmp(&a, &b, sizeof(a)) == 0; };
    // A root's own transform, as it is.
    CHECK(equal(hierarchy::cachedWorldTransform(root),
                *root.getComponent<TransformComponent>()));
    CHECK(equal(hierarchy::cachedWorldTransform(child),
                hierarchy::worldTransform(child)));
    CHECK(equal(hierarchy::cachedWorldTransform(child),
                TransformComponent::fromMatrix(cacheOf(child).matrix())));
  }

  TEST("SFS_CHECK_TRANSFORMS should catch a transform written after the pass")
  {
    World w;
    Entity root = w.placed();
    Entity child = w.placed(root);
    debugFeatures::allow(true);
    debugFeatures::setCheckTransforms(true);
    CHECK(debugFeatures::checkTransforms());
    w.frame();
    CHECK(!root.hasComponent<TransformCheckWarned>());
    CHECK(!child.hasComponent<TransformCheckWarned>());
    CHECK(hierarchy::checkWorldCache(child, cacheOf(child), "in a test"));

    // Written in the render phase, after the pass: the cached read is a
    // frame behind, and the check says so, once.
    root.getComponent<TransformComponent>()->position.x += 2.0f;
    hierarchy::cachedWorldMatrix(child);
    CHECK(child.hasComponent<TransformCheckWarned>());
    CHECK(!root.hasComponent<TransformCheckWarned>());
    CHECK(!hierarchy::checkWorldCache(root, cacheOf(root), "in a test"));
    CHECK(root.hasComponent<TransformCheckWarned>());

    // Said once per entity: a second stale read of it isn't logged again.
    int warnings = 0;
    const Logger::SinkId sink = Logger::addSink(
        [&](Logger::Level level, const std::string& message)
        {
          if (level == Logger::Level::WARN &&
              message.find("SFS_CHECK_TRANSFORMS") != std::string::npos)
            ++warnings;
        });
    CHECK(!hierarchy::checkWorldCache(child, cacheOf(child), "in a test"));
    hierarchy::cachedWorldMatrix(child);
    Logger::flush();
    CHECK(warnings == 0);
    // Another entity's first one is.
    Entity other = w.placed(root, 4.0f);
    w.frame();
    root.getComponent<TransformComponent>()->position.y += 1.0f;
    hierarchy::cachedWorldMatrix(other);
    Logger::flush();
    CHECK(warnings == 1);
    Logger::removeSink(sink);

    // A destroyed entity has nothing to check, and isn't tagged.
    WorldTransformComponent kept = cacheOf(other);
    w.scene.destroyEntity(other);
    w.frame();
    CHECK(hierarchy::checkWorldCache(other, kept, "in a test"));

    // The next pass puts them right.
    w.frame();
    CHECK(hierarchy::checkWorldCache(child, cacheOf(child), "in a test"));
    debugFeatures::setCheckTransforms(false);
    CHECK(!debugFeatures::checkTransforms());
  }

  return testing::report("transformSystemTests");
}
