#include "engine/core/hierarchy/hierarchy.h"

#include "engine/core/components/parentComponent.h"
#include "engine/core/components/transform3DComponent.h"
#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "glm/glm/matrix.hpp"

#include <unordered_set>

namespace sfs::hierarchy
{

namespace
{
// Guards walks up the parent chain against a loop made by writing
// ParentComponent directly (setParent refuses them).
constexpr int kMaxDepth = 1024;

glm::mat4 localMatrix(const Entity& entity)
{
  return entity.hasComponent<Transform3DComponent>()
             ? entity.getComponent<Transform3DComponent>().matrix()
             : glm::mat4(1.0f);
}
} // namespace

Entity parentOf(const Entity& entity)
{
  if (!entity.hasComponent<ParentComponent>())
    return {};
  const Entity parent = entity.getComponent<ParentComponent>().parent;
  return parent.isValid() && parent.isAlive() ? parent : Entity();
}

bool isAncestor(const Entity& ancestor, const Entity& entity)
{
  Entity current = parentOf(entity);
  for (int depth = 0; current.isValid() && depth < kMaxDepth; ++depth)
  {
    if (current == ancestor)
      return true;
    current = parentOf(current);
  }
  return false;
}

bool canParent(const Entity& child, const Entity& parent)
{
  if (!parent.isValid())
    return true;
  return parent != child && !isAncestor(child, parent);
}

bool setParent(Entity child, const Entity& parent)
{
  if (!canParent(child, parent))
    return false;

  if (!parent.isValid())
  {
    if (child.hasComponent<ParentComponent>())
      child.removeComponent<ParentComponent>();
    return true;
  }

  if (child.hasComponent<ParentComponent>())
    child.getComponent<ParentComponent>().parent = parent;
  else
    child.addComponent<ParentComponent>(parent);
  return true;
}

bool setParentKeepWorld(Entity child, const Entity& parent)
{
  const glm::mat4 world = worldMatrix(child);
  if (!setParent(child, parent))
    return false;
  if (child.hasComponent<Transform3DComponent>())
    setWorldMatrix(child, world);
  return true;
}

glm::mat4 worldMatrix(const Entity& entity)
{
  glm::mat4 world = localMatrix(entity);
  Entity current = parentOf(entity);
  for (int depth = 0; current.isValid() && depth < kMaxDepth; ++depth)
  {
    world = localMatrix(current) * world;
    current = parentOf(current);
  }
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
  entity.getComponent<Transform3DComponent>() =
      Transform3DComponent::fromMatrix(glm::inverse(parentWorldMatrix(entity)) *
                                       world);
}

EntityTree::EntityTree(const std::vector<Entity>& entities)
{
  std::unordered_set<Entity::EntityId> listed;
  for (const Entity& entity : entities)
    listed.insert(entity.getId());

  for (const Entity& entity : entities)
  {
    const Entity parent = parentOf(entity);
    // A parent outside the list, or a loop, makes it a root so it's still
    // reached from the top.
    if (parent.isValid() && listed.contains(parent.getId()) &&
        !isAncestor(entity, entity))
      m_children[parent.getId()].push_back(entity);
    else
      m_roots.push_back(entity);
  }
}

const std::vector<Entity>& EntityTree::children(const Entity& entity) const
{
  static const std::vector<Entity> kNone;
  const auto it = m_children.find(entity.getId());
  return it == m_children.end() ? kNone : it->second;
}

std::vector<Entity> EntityTree::preOrder() const
{
  std::vector<Entity> out;
  for (const Entity& root : m_roots)
    appendSubtree(root, out);
  return out;
}

std::vector<Entity> EntityTree::subtree(const Entity& root) const
{
  std::vector<Entity> out;
  appendSubtree(root, out);
  return out;
}

void EntityTree::appendSubtree(const Entity& root,
                               std::vector<Entity>& out) const
{
  out.push_back(root);
  for (const Entity& child : children(root))
    appendSubtree(child, out);
}

} // namespace sfs::hierarchy
