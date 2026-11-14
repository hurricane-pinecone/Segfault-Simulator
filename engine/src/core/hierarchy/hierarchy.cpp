#include "engine/core/hierarchy/hierarchy.h"

#include "engine/core/components/nameComponent.h"
#include "engine/core/components/restPoseComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/components/worldTransformComponent.h"
#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/core/ecs/expect.h"
#include "engine/core/logger/logger.h"
#include "engine/core/util/debugFeatures.h"
#include "engine/core/util/hash.h"
#include "glm/glm/ext/matrix_float3x3.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include "glm/glm/gtc/quaternion.hpp"
#include "glm/glm/matrix.hpp"

#include <cstring>
#include <string>

namespace sfs::hierarchy
{

namespace
{
constexpr Entity::EntityId kNone = HierarchyLinks::kNone;

// How many levels a world walk keeps on the stack; a deeper chain is walked
// in parts of this many.
constexpr int kChainLevels = 32;

// One level of a world walk: an entity with a transform, and its world cache
// once it's been looked at. Plain data, so a walk's array costs nothing to
// make.
struct Level
{
  Entity::EntityId id;
  const TransformComponent* local;
  const WorldTransformComponent* cache;
};

// What composing a chain gives: the world matrix of the nearest entity with a
// transform at or above where it began (`holder`; kNone, and `world` unset,
// when there's none), and whether every level down to it was clean, which
// makes `world` that entity's cache.
struct Composed
{
  glm::mat4x3 world;
  Entity::EntityId holder = kNone;
  bool clean = true;
};

// The world matrix of the living entity `id`, composed from the top down.
// A level is clean when its cache was made under the same nearest ancestor
// with a transform and from the same local transform as now. Above the
// highest level that isn't, every cache is what the last transform pass
// composed from the transforms as they are now, so composing starts from the
// cache of the clean level just above it. Caches are looked at from the top
// down only as far as that level. The walk keeps up to kChainLevels levels on
// the stack and composes the part above those first.
void composeChain(Registry& registry,
                  Entity::EntityId id,
                  bool useCaches,
                  Composed& out)
{
  const std::vector<HierarchyNode>& nodes = registry.links().nodes;
  // The links hold living ids only.
  const auto transforms = registry.finder<TransformComponent>();
  Level chain[kChainLevels];
  int levels = 0;
  Entity::EntityId at = id;
  for (; at != kNone && levels < kChainLevels; at = nodes[at].parent)
    if (const TransformComponent* local = transforms(at))
      chain[levels++] = {at, local, nullptr};
  if (at != kNone)
    composeChain(registry, at, useCaches, out);
  else
    out = {};
  if (levels == 0)
    return;

  int top = levels - 1;
  if (out.clean && useCaches)
  {
    const auto caches = registry.finder<WorldTransformComponent>();
    for (Entity::EntityId above = out.holder; top >= 0; --top)
    {
      Level& level = chain[top];
      level.cache = caches(level.id);
      if (!level.cache ||
          !cacheCurrent(*level.cache, transformHash(*level.local), above))
        break;
      above = level.id;
    }
    if (top < 0)
    {
      out.world = chain[0].cache->world;
      out.holder = chain[0].id;
      return;
    }
  }
  const glm::mat4x3* base = top + 1 < levels      ? &chain[top + 1].cache->world
                            : out.holder != kNone ? &out.world
                                                  : nullptr;
  glm::mat4x3 world = composeWorld(base, *chain[top].local);
  for (int level = top - 1; level >= 0; --level)
    world = composeWorld(&world, *chain[level].local);
  out.world = world;
  out.holder = chain[0].id;
  out.clean = false;
}

// composeChain() for an entity; `holder` kNone for a destroyed one.
Composed composedOf(const Entity& entity, bool useCaches)
{
  Composed composed;
  if (entity.isValid())
    composeChain(*entity.registryOf(), entity.getId(), useCaches, composed);
  return composed;
}

// The entity's world transform: a root's own, else its world matrix (as
// `world` gives it) taken apart.
template <typename WorldOf>
TransformComponent worldTransformOf(const Entity& entity, WorldOf world)
{
  const TransformComponent* local = entity.getComponent<TransformComponent>();
  if (!expect(local, entity))
    return {};
  if (!parentOf(entity).isValid())
    return *local;
  return TransformComponent::fromMatrix(world(entity));
}

// The entity's links, or null for a destroyed or stale handle.
const HierarchyNode* nodeOf(const Entity& entity)
{
  if (!entity.isValid())
    return nullptr;
  return &entity.registryOf()->links().nodes[entity.getId()];
}
} // namespace

Entity parentOf(const Entity& entity)
{
  const HierarchyNode* node = nodeOf(entity);
  if (!node || node->parent == kNone)
    return {};
  return entity.registryOf()->getEntity(node->parent);
}

Entity rootOf(const Entity& entity)
{
  if (!entity.isValid())
    return {};
  const std::vector<HierarchyNode>& nodes = entity.registryOf()->links().nodes;
  Entity::EntityId at = entity.getId();
  while (nodes[at].parent != kNone)
    at = nodes[at].parent;
  return at == entity.getId() ? entity : entity.registryOf()->getEntity(at);
}

bool isAncestor(const Entity& ancestor, const Entity& entity)
{
  if (!entity.isValid() || !ancestor.isValid() ||
      ancestor.registryOf() != entity.registryOf())
    return false;
  const std::vector<HierarchyNode>& nodes = entity.registryOf()->links().nodes;
  for (Entity::EntityId at = nodes[entity.getId()].parent; at != kNone;
       at = nodes[at].parent)
    if (at == ancestor.getId())
      return true;
  return false;
}

bool canParent(const Entity& child, const Entity& parent)
{
  // No parent at all (a default-made handle): a root, always.
  if (!parent.registryOf() || parent.getId() == Entity::InvalidId)
    return true;
  if (!child.isValid() || parent.registryOf() != child.registryOf())
    return false;
  // A destroyed parent makes the child a root.
  return !parent.isValid() || (parent != child && !isAncestor(child, parent));
}

bool setParent(Entity child, const Entity& parent)
{
  if (!canParent(child, parent))
  {
    const std::string which =
        "hierarchy::setParent: entity " + std::to_string(child.getId());
    if (!child.isValid())
      LOG_ERROR(which + " was destroyed, so it can't be put under a parent");
    else if (parent.registryOf() != child.registryOf())
      LOG_ERROR(which + " and its new parent are in different registries");
    return false;
  }
  if (!child.isValid())
    return true; // made a root: there's nothing to take it out of
  child.registryOf()->relink(
      child.getId(), parent.isValid() ? parent.getId() : kNone);
  return true;
}

bool setParentKeepWorld(Entity child, const Entity& parent)
{
  const glm::mat4 world = worldMatrix(child);
  if (!setParent(child, parent))
    return false;
  if (child.hasComponent<TransformComponent>())
    setWorldMatrix(child, world);
  return true;
}

bool hasChildren(const Entity& entity)
{
  const HierarchyNode* node = nodeOf(entity);
  return node && node->firstChild != kNone;
}

ChildRange childrenOf(const Entity& entity)
{
  const HierarchyNode* node = nodeOf(entity);
  return node ? ChildRange(entity.registryOf(), node->firstChild)
              : ChildRange();
}

DescendantRange::iterator& DescendantRange::iterator::operator++()
{
  const std::vector<HierarchyNode>& nodes = m_registry->links().nodes;
  if (!m_skipChildren && nodes[m_id].firstChild != kNone)
  {
    m_id = nodes[m_id].firstChild;
    return *this;
  }
  m_skipChildren = false;
  // An entity reparented out of the walk, or removed, during it ends the
  // climb at a root (kNone above it) instead of at m_root.
  for (Entity::EntityId at = m_id; at != m_root && at != kNone;
       at = nodes[at].parent)
    if (nodes[at].nextSibling != kNone)
    {
      m_id = nodes[at].nextSibling;
      return *this;
    }
  m_id = kNone;
  return *this;
}

DescendantRange::iterator DescendantRange::begin() const
{
  return {m_registry,
          m_root,
          m_registry ? m_registry->links().nodes[m_root].firstChild : kNone};
}

DescendantRange descendants(const Entity& root)
{
  return root.isValid() ? DescendantRange(root.registryOf(), root.getId())
                        : DescendantRange();
}

RootRange::iterator::iterator(Registry* registry, Entity::EntityId id)
    : m_registry(registry), m_id(id)
{
  settle();
}

RootRange::iterator& RootRange::iterator::operator++()
{
  ++m_id;
  settle();
  return *this;
}

void RootRange::iterator::settle()
{
  if (m_id == kNone)
    return;
  const std::vector<HierarchyNode>& nodes = m_registry->links().nodes;
  // A free id's node is all kNone too: getEntity() tells it isn't alive.
  while (m_id < nodes.size() && (nodes[m_id].parent != kNone ||
                                 !m_registry->getEntity(m_id).isValid()))
    ++m_id;
  if (m_id >= nodes.size())
    m_id = kNone;
}

RootRange roots(Registry& registry) { return RootRange(registry); }

const std::vector<HierarchyLinks::Placed>& preorder(Registry& registry)
{
  return registry.preorder();
}

glm::mat4x3 composeWorld(const glm::mat4x3* parentWorld,
                         const TransformComponent& local)
{
  // TransformComponent::matrix()'s operations: glm's mat4_cast is mat3_cast
  // widened.
  glm::mat3 axes = glm::mat3_cast(local.rotation);
  axes[0] *= local.scale.x;
  axes[1] *= local.scale.y;
  axes[2] *= local.scale.z;
  if (!parentWorld)
    return glm::mat4x3(axes[0], axes[1], axes[2], local.position);
  // Four lanes wide, so each column is a vector multiply-add per axis.
  const glm::mat4x3& parent = *parentWorld;
  const glm::vec4 x(parent[0], 0.0f);
  const glm::vec4 y(parent[1], 0.0f);
  const glm::vec4 z(parent[2], 0.0f);
  const glm::vec3& at = local.position;
  return glm::mat4x3(
      glm::vec3(x * axes[0].x + y * axes[0].y + z * axes[0].z),
      glm::vec3(x * axes[1].x + y * axes[1].y + z * axes[1].z),
      glm::vec3(x * axes[2].x + y * axes[2].y + z * axes[2].z),
      glm::vec3(x * at.x + y * at.y + z * at.z + glm::vec4(parent[3], 0.0f)));
}

uint64_t transformHash(const TransformComponent& local)
{
  static_assert(sizeof(TransformComponent) == 40,
                "a transform is hashed as five 64-bit words, with no padding");
  uint64_t words[5];
  std::memcpy(words, &local, sizeof(words));
  return hashWords(words, 5);
}

glm::mat4 worldMatrix(const Entity& entity)
{
  const Composed composed = composedOf(entity, true);
  return composed.holder == kNone ? glm::mat4(1.0f) : glm::mat4(composed.world);
}

glm::mat4 cachedWorldMatrix(const Entity& entity,
                            const WorldTransformComponent& cache,
                            bool check)
{
  if (cache.version == 0)
    return worldMatrix(entity);
  if (check)
    checkWorldCache(entity, cache, "read in the render phase");
  return cache.matrix();
}

glm::mat4 cachedWorldMatrix(const Entity& entity)
{
  const WorldTransformComponent* cache =
      entity.isValid()
          ? entity.registryOf()->find<WorldTransformComponent>(entity.getId())
          : nullptr;
  return cache ? cachedWorldMatrix(entity, *cache) : worldMatrix(entity);
}

TransformComponent cachedWorldTransform(const Entity& entity)
{
  return worldTransformOf(
      entity, [](const Entity& e) { return cachedWorldMatrix(e); });
}

bool checkWorldCache(const Entity& entity,
                     const WorldTransformComponent& cache,
                     const char* phase)
{
  if (!entity.isValid())
    return true;
  const Composed composed = composedOf(entity, false);
  if (composed.holder != kNone &&
      std::memcmp(&composed.world, &cache.world, sizeof(cache.world)) == 0)
    return true;
  if (entity.hasComponent<TransformCheckWarned>())
    return false;
  const NameComponent* named = entity.getComponent<NameComponent>();
  const std::string name = named ? " '" + named->name + "'" : std::string();
  LOG_WARN("SFS_CHECK_TRANSFORMS: entity " + std::to_string(entity.getId()) +
           name + ": its cached world matrix, " + phase +
           ", differs from one composed afresh. Was its transform, or an "
           "ancestor's, written after the transform pass?");
  Entity(entity).addComponent<TransformCheckWarned>();
  return false;
}

glm::mat4 parentWorldMatrix(const Entity& entity)
{
  const Entity parent = parentOf(entity);
  return parent.isValid() ? worldMatrix(parent) : glm::mat4(1.0f);
}

glm::vec3 worldPosition(const Entity& entity)
{
  return glm::vec3(worldMatrix(entity)[3]);
}

void setWorldMatrix(Entity entity, const glm::mat4& world)
{
  TransformComponent* local = entity.getComponent<TransformComponent>();
  if (!expect(local, entity))
    return;
  *local = TransformComponent::fromMatrix(
      glm::inverse(parentWorldMatrix(entity)) * world);
}

TransformComponent worldTransform(const Entity& entity)
{
  return worldTransformOf(
      entity, [](const Entity& e) { return worldMatrix(e); });
}

void setWorldTransform(Entity entity, const TransformComponent& world)
{
  if (parentOf(entity).isValid())
    setWorldMatrix(entity, world.matrix());
  else if (TransformComponent* local =
               entity.getComponent<TransformComponent>();
           expect(local, entity))
    *local = world;
}

void recordRestPose(Entity entity)
{
  const TransformComponent* transform =
      entity.getComponent<TransformComponent>();
  if (!transform)
    return;
  RestPoseComponent* rest = entity.getComponent<RestPoseComponent>();
  if (!rest)
  {
    entity.addComponent<RestPoseComponent>();
    rest = entity.getComponent<RestPoseComponent>();
  }
  rest->position = transform->position;
  rest->rotation = transform->rotation;
  rest->scale = transform->scale;
}

} // namespace sfs::hierarchy
