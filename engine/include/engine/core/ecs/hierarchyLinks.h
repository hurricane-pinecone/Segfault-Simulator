#pragma once

// The parent/child structure of a registry's entities, kept by the Registry
// beside its other per-id arrays. Read it through hierarchy.h; only
// hierarchy::setParent and the flush that removes an entity change it.

#include "engine/core/ecs/entity.h"

#include <cstdint>
#include <vector>

namespace sfs
{

/**
 * One entity's links, by its id: 16 bytes, so a walk down or up the tree is
 * one cache access per hop. Siblings are kept in id order.
 */
struct HierarchyNode
{
  /** The parent's id; HierarchyLinks::kNone for a root. */
  Entity::EntityId parent;
  /** The first (lowest id) child's id; kNone when it has none. */
  Entity::EntityId firstChild;
  /** The next sibling's id; kNone for the last child (and for a root). */
  Entity::EntityId nextSibling;
  /**
   * The previous sibling's id. A first child's is its parent's last child, so
   * appending needs no other field. kNone for a root.
   */
  Entity::EntityId prevSibling;
};

/**
 * The hierarchy of a registry: its links by entity id, a version that moves
 * with every link change, and a preorder index built from them on demand.
 */
struct HierarchyLinks
{
  /** No entity: a root's parent, a leaf's first child, a last sibling. */
  static constexpr Entity::EntityId kNone = Entity::InvalidId;

  /** An entry of the preorder index: 8 bytes. */
  struct Placed
  {
    /** The entity's id. */
    Entity::EntityId id;
    /** The index of its parent's entry in the preorder; kNone for a root. */
    uint32_t parentPos;
  };

  /** Each entity's links, by id; sized with the registry's per-id arrays. */
  std::vector<HierarchyNode> nodes;
  /**
   * Goes up whenever a link changes (setParent, or an entity with a parent or
   * children removed at a flush). Creating an entity doesn't move it.
   */
  uint64_t linkVersion = 0;
  /**
   * Every living entity, parents before children, roots and siblings in id
   * order: the order scenes save in. Rebuilt (reusing its capacity) only
   * when it's asked for after the links or the set of entities changed.
   */
  std::vector<Placed> preorder;
  /** linkVersion plus the registry's entity-set version at the last build. */
  uint64_t preorderBuiltAt = ~0ull;
};

} // namespace sfs
