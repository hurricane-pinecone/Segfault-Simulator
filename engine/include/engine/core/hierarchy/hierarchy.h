#pragma once

// Parent/child links between entities (ParentComponent) and the world
// transforms they imply. A child's Transform3DComponent is relative to its
// parent, so its world matrix is the product of the local matrices up its
// chain of parents; an ancestor without a Transform3DComponent contributes
// nothing (it only groups).
//
// World matrices are computed on demand, walking up the chain, so they are
// never stale: move a parent and its children's next worldMatrix() follows.
//
// Destroying an entity doesn't destroy its children; they become roots. Use
// EntityTree::subtree() to destroy a branch.

#include "engine/core/ecs/entity.h"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_float3.hpp"

#include <unordered_map>
#include <vector>

namespace sfs::hierarchy
{

/** The entity's parent; invalid for a root (or if the parent was destroyed). */
Entity parentOf(const Entity& entity);

/** True if `ancestor` is `entity`'s parent, grandparent, and so on. */
bool isAncestor(const Entity& ancestor, const Entity& entity);

/** True if `child` can be put under `parent` (an invalid parent means root). */
bool canParent(const Entity& child, const Entity& parent);

/**
 * Put `child` under `parent`, or make it a root if `parent` is invalid. Its
 * local transform is kept, so it moves to sit relative to the new parent.
 *
 * @return false (nothing changes) if that would make a loop.
 */
bool setParent(Entity child, const Entity& parent);

/**
 * Like setParent, but the child stays where it is in the world: its local
 * transform is recomputed against the new parent. A parent with shear (a
 * rotated child of a non-uniformly scaled parent) can't be matched exactly.
 */
bool setParentKeepWorld(Entity child, const Entity& parent);

/** Local-to-world matrix: the parents' matrices times the entity's own. */
glm::mat4 worldMatrix(const Entity& entity);

/** The parent's world matrix (identity for a root). */
glm::mat4 parentWorldMatrix(const Entity& entity);

/** The entity's world position (its Transform3DComponent's origin). */
glm::vec3 worldPosition(const Entity& entity);

/**
 * Set the local transform so the entity's world matrix becomes `world`.
 * Needs a Transform3DComponent.
 */
void setWorldMatrix(Entity entity, const glm::mat4& world);

/**
 * A snapshot of the parent/child structure of a set of entities, for walking
 * it top-down. Siblings keep the order of the list it was built from (a
 * scene's entities(): id order). An entity whose parent isn't in the list is a
 * root.
 */
class EntityTree
{
public:
  explicit EntityTree(const std::vector<Entity>& entities);

  const std::vector<Entity>& roots() const { return m_roots; }
  const std::vector<Entity>& children(const Entity& entity) const;

  /** Every entity, each parent before its children (the order scenes save in). */
  std::vector<Entity> preOrder() const;

  /** `root` and everything below it, parents first. */
  std::vector<Entity> subtree(const Entity& root) const;

private:
  void appendSubtree(const Entity& root, std::vector<Entity>& out) const;

  std::vector<Entity> m_roots;
  std::unordered_map<Entity::EntityId, std::vector<Entity>> m_children;
};

} // namespace sfs::hierarchy
