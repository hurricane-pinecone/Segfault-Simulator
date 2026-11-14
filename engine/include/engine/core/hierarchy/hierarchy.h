#pragma once

// Parent/child links between entities and the world transforms they imply.
// The registry keeps the links (HierarchyLinks): each entity's parent, first
// child and siblings, siblings in id order. They change at setParent, at once,
// and when the flush removes an entity.
//
// A child's TransformComponent is relative to its parent, so its world matrix
// is the product of the local matrices up its chain of parents; an ancestor
// without a TransformComponent contributes nothing (it only groups). World
// matrices are computed on demand, walking up the chain, so they are never
// stale: move a parent and its children's next worldMatrix() follows.
//
// Destroying an entity doesn't destroy its children; they become roots at the
// flush. Scene::destroyBranch() destroys a branch.

#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/hierarchyLinks.h"
#include "engine/core/ecs/registry.h"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_float3.hpp"

#include <cstddef>
#include <iterator>
#include <vector>

namespace sfs::hierarchy
{

/** The entity's parent; invalid for a root, a destroyed or a stale handle. */
Entity parentOf(const Entity& entity);

/** True if `ancestor` is `entity`'s parent, grandparent, and so on. */
bool isAncestor(const Entity& ancestor, const Entity& entity);

/**
 * True if setParent(child, parent) would accept it. Making an entity a root
 * (an invalid parent) always is. Otherwise `child` must be alive, in the
 * parent's registry, and not the parent or above it.
 */
bool canParent(const Entity& child, const Entity& parent);

/**
 * Put `child` under `parent`, or make it a root if `parent` is invalid or
 * destroyed. Its local transform is kept, so it moves to sit relative to the
 * new parent. It takes its place among the parent's children in id order,
 * and childrenOf() lists it at once.
 *
 * @return false (nothing changes) if canParent() refuses it: a loop, a
 *         destroyed child given a valid parent, or a parent in another
 *         registry (the last two logged). Making a destroyed entity a root
 *         does nothing and returns true.
 */
bool setParent(Entity child, const Entity& parent);

/**
 * Like setParent, but the child stays where it is in the world: its local
 * transform is recomputed against the new parent. A parent with shear (a
 * rotated child of a non-uniformly scaled parent) can't be matched exactly.
 */
bool setParentKeepWorld(Entity child, const Entity& parent);

/** Whether the entity has children (false for a destroyed or stale handle). */
bool hasChildren(const Entity& entity);

/**
 * An entity's children, in id order, as a forward range of Entity. It reads
 * the registry's links as it goes and allocates nothing. Reparenting the
 * children while walking it isn't safe: walk a copy (destroying is, as it
 * waits for the flush).
 */
class ChildRange
{
public:
  class iterator
  {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = Entity;
    using difference_type = std::ptrdiff_t;
    using pointer = void;
    using reference = Entity;

    iterator() = default;
    iterator(Registry* registry, Entity::EntityId id)
        : m_registry(registry), m_id(id)
    {
    }

    /** The child here. */
    Entity operator*() const { return m_registry->getEntity(m_id); }
    iterator& operator++()
    {
      m_id = m_registry->links().nodes[m_id].nextSibling;
      return *this;
    }
    iterator operator++(int)
    {
      iterator before = *this;
      ++*this;
      return before;
    }
    bool operator==(const iterator& other) const { return m_id == other.m_id; }

  private:
    Registry* m_registry = nullptr;
    Entity::EntityId m_id = HierarchyLinks::kNone;
  };

  ChildRange() = default;
  ChildRange(Registry* registry, Entity::EntityId first)
      : m_registry(registry), m_first(first)
  {
  }

  iterator begin() const { return {m_registry, m_first}; }
  iterator end() const { return {m_registry, HierarchyLinks::kNone}; }
  /** Whether there are no children. */
  bool empty() const { return m_first == HierarchyLinks::kNone; }

private:
  Registry* m_registry = nullptr;
  Entity::EntityId m_first = HierarchyLinks::kNone;
};

/**
 * The entity's children, in id order; empty for a destroyed or stale handle.
 * Cost: O(1) to make, O(children) to walk, no allocation.
 */
ChildRange childrenOf(const Entity& entity);

/**
 * The first child (in id order) with a `Tag` component.
 * @return the child, or an invalid entity if none has one
 */
template <typename Tag>
Entity findChild(const Entity& entity)
{
  for (const Entity& child : childrenOf(entity))
    if (child.hasComponent<Tag>())
      return child;
  return {};
}

/**
 * Every child with a `Tag` component, in id order, into `out` (cleared
 * first), so a reused buffer allocates nothing.
 */
template <typename Tag>
void findChildren(const Entity& entity, std::vector<Entity>& out)
{
  out.clear();
  for (const Entity& child : childrenOf(entity))
    if (child.hasComponent<Tag>())
      out.push_back(child);
}

/**
 * Everything below an entity (the entity itself not included), parents
 * before children, siblings in id order. The walk keeps no stack: the next
 * entity is the current one's first child, else the next sibling of the
 * nearest of it and its ancestors below the root that has one. Its iterator's
 * skipChildren() leaves out what is below the current entity (a nested
 * machine, say). As with ChildRange, reparent while walking a copy.
 */
class DescendantRange
{
public:
  class iterator
  {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = Entity;
    using difference_type = std::ptrdiff_t;
    using pointer = void;
    using reference = Entity;

    iterator() = default;
    iterator(Registry* registry, Entity::EntityId root, Entity::EntityId id)
        : m_registry(registry), m_root(root), m_id(id)
    {
    }

    /** The entity here. */
    Entity operator*() const { return m_registry->getEntity(m_id); }
    iterator& operator++();
    iterator operator++(int)
    {
      iterator before = *this;
      ++*this;
      return before;
    }
    bool operator==(const iterator& other) const { return m_id == other.m_id; }

    /** The next step goes past what is below the current entity. */
    void skipChildren() { m_skipChildren = true; }

  private:
    Registry* m_registry = nullptr;
    Entity::EntityId m_root = HierarchyLinks::kNone;
    Entity::EntityId m_id = HierarchyLinks::kNone;
    bool m_skipChildren = false;
  };

  DescendantRange() = default;
  DescendantRange(Registry* registry, Entity::EntityId root)
      : m_registry(registry), m_root(root)
  {
  }

  iterator begin() const;
  iterator end() const { return {m_registry, m_root, HierarchyLinks::kNone}; }

private:
  Registry* m_registry = nullptr;
  Entity::EntityId m_root = HierarchyLinks::kNone;
};

/**
 * Everything below `root` (not `root` itself), parents first; empty for a
 * destroyed or stale handle. Cost: O(entities visited), no allocation.
 */
DescendantRange descendants(const Entity& root);

/**
 * The first entity below `root`, in preorder, with a `Tag` component.
 * @return it, or an invalid entity if none has one
 */
template <typename Tag>
Entity findDescendant(const Entity& root)
{
  for (const Entity& entity : descendants(root))
    if (entity.hasComponent<Tag>())
      return entity;
  return {};
}

/**
 * The registry's root entities (those with no parent), in id order, as a
 * forward range of Entity. It reads the registry as it goes (no list is
 * kept), so it's never stale; creating entities while walking it is safe.
 */
class RootRange
{
public:
  class iterator
  {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = Entity;
    using difference_type = std::ptrdiff_t;
    using pointer = void;
    using reference = Entity;

    iterator() = default;
    /** At the first root at or after `id` (the end if there is none). */
    iterator(Registry* registry, Entity::EntityId id);

    /** The root here. */
    Entity operator*() const { return m_registry->getEntity(m_id); }
    iterator& operator++();
    iterator operator++(int)
    {
      iterator before = *this;
      ++*this;
      return before;
    }
    bool operator==(const iterator& other) const { return m_id == other.m_id; }

  private:
    // Moves on to the first root at or after m_id.
    void settle();

    Registry* m_registry = nullptr;
    Entity::EntityId m_id = HierarchyLinks::kNone;
  };

  explicit RootRange(Registry& registry) : m_registry(&registry) {}

  iterator begin() const { return {m_registry, 0}; }
  iterator end() const { return {m_registry, HierarchyLinks::kNone}; }

private:
  Registry* m_registry;
};

/**
 * The registry's root entities, in id order. Cost: O(entity ids) for a whole
 * walk, no allocation.
 */
RootRange roots(Registry& registry);

/**
 * Every living entity of the registry, parents before children, roots and
 * siblings in id order (the order scenes save in), each with its parent's
 * place in the list (HierarchyLinks::kNone for a root). The registry rebuilds
 * it only when it's asked for after a link changed or an entity was created
 * or removed; otherwise this is a read. The rebuild rewrites the list in
 * place, so while iterating it, call nothing that may call preorder() (a
 * save, findEntity). Main thread only.
 *
 * @param registry the entities' registry
 * @return the registry's index
 */
const std::vector<HierarchyLinks::Placed>& preorder(Registry& registry);

/** Local-to-world matrix: the parents' matrices times the entity's own. */
glm::mat4 worldMatrix(const Entity& entity);

/** The parent's world matrix (identity for a root). */
glm::mat4 parentWorldMatrix(const Entity& entity);

/** The entity's world position (its TransformComponent's origin). */
glm::vec3 worldPosition(const Entity& entity);

/**
 * Set the local transform so the entity's world matrix becomes `world`.
 * Needs a TransformComponent.
 */
void setWorldMatrix(Entity entity, const glm::mat4& world);

/**
 * The entity's transform in the world: its own for a root, else its world
 * matrix taken apart (see TransformComponent::fromMatrix). What 2D code reads
 * a world position2D(), height (position.z), yaw() and scale2D() from.
 * Needs a TransformComponent.
 */
TransformComponent worldTransform(const Entity& entity);

/**
 * Set the local transform so the entity's world transform becomes `world`
 * (exactly, for a root). Needs a TransformComponent.
 */
void setWorldTransform(Entity entity, const TransformComponent& world);

/**
 * Record the entity's TransformComponent (its local pose) as its
 * RestPoseComponent, adding one if missing or refreshing it. Does nothing to
 * an entity without a TransformComponent.
 *
 * @param entity The entity to record.
 */
void recordRestPose(Entity entity);

} // namespace sfs::hierarchy
