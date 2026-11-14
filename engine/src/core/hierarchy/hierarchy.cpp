#include "engine/core/hierarchy/hierarchy.h"

#include "engine/core/components/restPoseComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/core/logger/logger.h"
#include "glm/glm/matrix.hpp"

#include <string>

namespace sfs::hierarchy
{

namespace
{
constexpr Entity::EntityId kNone = HierarchyLinks::kNone;

glm::mat4 localMatrix(const Entity& entity)
{
  return entity.hasComponent<TransformComponent>()
             ? entity.getComponent<TransformComponent>().matrix()
             : glm::mat4(1.0f);
}

// The entity's links, or null for a destroyed or stale handle.
const HierarchyNode* nodeOf(const Entity& entity)
{
  if (!entity.isAlive())
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

bool isAncestor(const Entity& ancestor, const Entity& entity)
{
  if (!entity.isAlive() || !ancestor.isAlive() ||
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
  if (!parent.isValid())
    return true;
  return child.isAlive() && parent.registryOf() == child.registryOf() &&
         parent != child && !isAncestor(child, parent);
}

bool setParent(Entity child, const Entity& parent)
{
  if (!canParent(child, parent))
  {
    const std::string which =
        "hierarchy::setParent: entity " + std::to_string(child.getId());
    if (!child.isAlive())
      LOG_ERROR(which + " was destroyed, so it can't be put under a parent");
    else if (parent.registryOf() != child.registryOf())
      LOG_ERROR(which + " and its new parent are in different registries");
    return false;
  }
  if (!child.isAlive())
    return true; // made a root: there's nothing to take it out of
  child.registryOf()->relink(
      child.getId(), parent.isAlive() ? parent.getId() : kNone);
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
  return root.isAlive() ? DescendantRange(root.registryOf(), root.getId())
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

glm::mat4 worldMatrix(const Entity& entity)
{
  glm::mat4 world = localMatrix(entity);
  for (Entity current = parentOf(entity); current.isValid();
       current = parentOf(current))
    world = localMatrix(current) * world;
  return world;
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
  entity.getComponent<TransformComponent>() = TransformComponent::fromMatrix(
      glm::inverse(parentWorldMatrix(entity)) * world);
}

TransformComponent worldTransform(const Entity& entity)
{
  const auto& local = entity.getComponent<TransformComponent>();
  if (!parentOf(entity).isValid())
    return local;
  return TransformComponent::fromMatrix(worldMatrix(entity));
}

void setWorldTransform(Entity entity, const TransformComponent& world)
{
  if (!parentOf(entity).isValid())
    entity.getComponent<TransformComponent>() = world;
  else
    setWorldMatrix(entity, world.matrix());
}

void recordRestPose(Entity entity)
{
  if (!entity.hasComponent<TransformComponent>())
    return;
  if (!entity.hasComponent<RestPoseComponent>())
    entity.addComponent<RestPoseComponent>();
  const TransformComponent& transform =
      entity.getComponent<TransformComponent>();
  RestPoseComponent& rest = entity.getComponent<RestPoseComponent>();
  rest.position = transform.position;
  rest.rotation = transform.rotation;
  rest.scale = transform.scale;
}

} // namespace sfs::hierarchy
