#pragma once

// Parent/child links between entities and the world transforms they imply.
// The registry keeps the links (HierarchyLinks): each entity's parent, first
// child and siblings, siblings in id order. They change at setParent, at once,
// and when the flush removes an entity.
//
// A child's TransformComponent is relative to its parent, so its world matrix
// is the product of the local matrices down its chain of parents, composed
// from the top by composeWorld(); an ancestor without a TransformComponent
// contributes nothing (it only groups).
//
// The transform pass (TransformSystem, first in every scene's render phase)
// caches each entity's world matrix in a WorldTransformComponent. worldMatrix()
// is never stale all the same: it checks the cache up the chain and composes
// only below the highest level that changed since the pass, so move a parent
// and its children's next worldMatrix() follows. Code that runs after the pass
// in the same frame (the render phase) reads the cache directly with
// cachedWorldMatrix().
//
// Destroying an entity doesn't destroy its children; they become roots at the
// flush. Scene::destroyBranch() destroys a branch.

#include "engine/core/components/transformComponent.h"
#include "engine/core/components/worldTransformComponent.h"
#include "engine/core/ecs/hierarchyLinks.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/util/debugFeatures.h"
#include "glm/glm/ext/matrix_float4x3.hpp"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_float3.hpp"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

namespace sfs::hierarchy
{

/** The entity's parent; invalid for a root, a destroyed or a stale handle. */
Entity parentOf(const Entity& entity);

/** True if `ancestor` is `entity`'s parent, grandparent, and so on. */
bool isAncestor(const Entity& ancestor, const Entity& entity);

/**
 * The top of an entity's branch (as Unity's Transform.root).
 *
 * @param entity the entity
 * @return its root ancestor, or the entity itself if it's a root; invalid for
 *         a destroyed or stale handle
 */
Entity rootOf(const Entity& entity);

/**
 * True if setParent(child, parent) would accept it. Making an entity a root
 * with a default-made parent always is. Otherwise `child` must be alive and
 * in the parent's registry, and a living parent must not be the child or
 * below it; a destroyed parent (of the child's registry) makes it a root.
 */
bool canParent(const Entity& child, const Entity& parent);

/**
 * Put `child` under `parent`, or make it a root if `parent` is default-made
 * or destroyed. Its local transform is kept, so it moves to sit relative to
 * the new parent. It takes its place among the parent's children in id order,
 * and childrenOf() lists it at once.
 *
 * @return false (nothing changes) if canParent() refuses it: a loop, a
 *         destroyed child given any parent but a default-made one, or a
 *         parent (living or destroyed) in another registry (the last two
 *         logged). Making a destroyed entity a root with a default-made
 *         parent does nothing and returns true.
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
 * An entity's children that match TTerms (as a query matches them: required
 * components, one Any<...> group, Excluding<...> groups), in id order, as the
 * rows a query gives (queryView.h): `for (auto [child, wheel] :
 * childrenOf<Wheel>(root))`, or `for (Wheel& wheel : ...)` without the
 * entity. Unlike a query it includes children still waiting for their first
 * flush, as childrenOf() does. It reads the links as it goes and allocates
 * nothing; per child, one signature test plus the fetches bound.
 * Reparenting the children, or adding or removing the terms' components,
 * while walking it isn't safe: walk a copy.
 */
template <typename... TTerms>
class ChildRows
{
public:
  class iterator
  {
  public:
    using iterator_concept = std::forward_iterator_tag;
    using iterator_category = std::input_iterator_tag;
    using value_type = QueryRow<TTerms...>;
    using difference_type = std::ptrdiff_t;
    using reference = QueryRow<TTerms...>;

    iterator() = default;
    iterator(Registry* registry, Entity::EntityId first)
        : m_registry(registry), m_fetch(*registry), m_id(first)
    {
      settle();
    }

    /** The row of the child here, valid for this step. */
    QueryRow<TTerms...> operator*() const { return {&m_current, &m_fetch}; }
    iterator& operator++()
    {
      m_id = m_registry->links().nodes[m_id].nextSibling;
      settle();
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
    // Moves on to the first matching child at or after m_id.
    void settle()
    {
      const QuerySignature& signature = detail::querySignature<TTerms...>();
      const std::vector<HierarchyNode>& nodes = m_registry->links().nodes;
      while (m_id != HierarchyLinks::kNone &&
             !m_registry->matches(m_id, signature))
        m_id = nodes[m_id].nextSibling;
      if (m_id != HierarchyLinks::kNone)
        m_current = m_registry->getEntity(m_id);
    }

    Registry* m_registry = nullptr;
    detail::Fetchers<TTerms...> m_fetch;
    Entity::EntityId m_id = HierarchyLinks::kNone;
    Entity m_current;
  };

  ChildRows() = default;
  ChildRows(Registry* registry, Entity::EntityId first)
      : m_registry(registry), m_first(first)
  {
  }

  iterator begin() const
  {
    return m_registry ? iterator(m_registry, m_first) : iterator();
  }
  iterator end() const { return {}; }
  /** Whether no child matches. */
  bool empty() const { return begin() == end(); }
  /**
   * The same rows without their entity: `for (auto [wheel, transform] :
   * childrenOf<Wheel, TransformComponent>(root).components())`.
   */
  ComponentView<ChildRows> components() const
  {
    return ComponentView<ChildRows>(*this);
  }

private:
  Registry* m_registry = nullptr;
  Entity::EntityId m_first = HierarchyLinks::kNone;
};

/**
 * The entity's children that match the terms, in id order, as rows (see
 * ChildRows); empty for a destroyed or stale handle.
 */
template <typename TTerm, typename... TTerms>
ChildRows<TTerm, TTerms...> childrenOf(const Entity& entity)
{
  if (!entity.isValid())
    return {};
  Registry* registry = entity.registryOf();
  return {registry, registry->links().nodes[entity.getId()].firstChild};
}

/**
 * The first child (in id order) that matches the terms (as childrenOf<...>()
 * matches them), e.g. findChild<Tag>(entity).
 * @return the child, or an invalid entity if none matches
 */
template <typename TTerm, typename... TTerms>
Entity findChild(const Entity& entity)
{
  const ChildRows<TTerm, TTerms...> rows = childrenOf<TTerm, TTerms...>(entity);
  const auto first = rows.begin();
  return first == rows.end() ? Entity{} : Entity((*first).entity());
}

/**
 * Every child that matches the terms, in id order, into `out` (cleared
 * first), so a reused buffer allocates nothing.
 */
template <typename TTerm, typename... TTerms>
void findChildren(const Entity& entity, std::vector<Entity>& out)
{
  out.clear();
  for (const Entity& child : childrenOf<TTerm, TTerms...>(entity))
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
    /** The id of the entity here (HierarchyLinks::kNone at the end). */
    Entity::EntityId id() const { return m_id; }
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
 * The entities below a root that match TTerms (as a query matches them), in
 * preorder, as the rows a query gives (queryView.h). It walks every entity
 * below the root and yields the matching ones; its iterator's skipChildren()
 * leaves out what is below the current row's entity. Pruning at entities that
 * don't match takes the untyped walk (descendants()). It includes entities
 * waiting for their first flush, and allocates nothing. As with ChildRows,
 * reparent or change the terms' components while walking a copy.
 */
template <typename... TTerms>
class DescendantRows
{
public:
  class iterator
  {
  public:
    using iterator_concept = std::forward_iterator_tag;
    using iterator_category = std::input_iterator_tag;
    using value_type = QueryRow<TTerms...>;
    using difference_type = std::ptrdiff_t;
    using reference = QueryRow<TTerms...>;

    iterator() = default;
    iterator(Registry* registry, Entity::EntityId root, Entity::EntityId first)
        : m_walk(registry, root, first), m_registry(registry),
          m_fetch(*registry)
    {
      settle();
    }

    /** The row of the entity here, valid for this step. */
    QueryRow<TTerms...> operator*() const { return {&m_current, &m_fetch}; }
    iterator& operator++()
    {
      ++m_walk;
      settle();
      return *this;
    }
    iterator operator++(int)
    {
      iterator before = *this;
      ++*this;
      return before;
    }
    bool operator==(const iterator& other) const
    {
      return m_walk == other.m_walk;
    }

    /** The next step goes past what is below the current row's entity. */
    void skipChildren() { m_walk.skipChildren(); }

  private:
    // Moves on to the first matching entity at or after the walk's.
    void settle()
    {
      const QuerySignature& signature = detail::querySignature<TTerms...>();
      while (m_walk.id() != HierarchyLinks::kNone &&
             !m_registry->matches(m_walk.id(), signature))
        ++m_walk;
      if (m_walk.id() != HierarchyLinks::kNone)
        m_current = m_registry->getEntity(m_walk.id());
    }

    DescendantRange::iterator m_walk;
    Registry* m_registry = nullptr;
    detail::Fetchers<TTerms...> m_fetch;
    Entity m_current;
  };

  DescendantRows() = default;
  DescendantRows(Registry* registry, Entity::EntityId root)
      : m_registry(registry), m_root(root)
  {
  }

  iterator begin() const
  {
    return m_registry ? iterator(m_registry,
                                 m_root,
                                 m_registry->links().nodes[m_root].firstChild)
                      : iterator();
  }
  iterator end() const { return {}; }
  /**
   * The same rows without their entity (its iterator has no
   * skipChildren()).
   */
  ComponentView<DescendantRows> components() const
  {
    return ComponentView<DescendantRows>(*this);
  }

private:
  Registry* m_registry = nullptr;
  Entity::EntityId m_root = HierarchyLinks::kNone;
};

/**
 * The entities below `root` that match the terms, parents first, as rows
 * (see DescendantRows); empty for a destroyed or stale handle.
 */
template <typename TTerm, typename... TTerms>
DescendantRows<TTerm, TTerms...> descendants(const Entity& root)
{
  return root.isValid()
             ? DescendantRows<TTerm, TTerms...>(root.registryOf(), root.getId())
             : DescendantRows<TTerm, TTerms...>();
}

/**
 * The first entity below `root`, in preorder, that matches the terms (as
 * descendants<...>() matches them), e.g. findDescendant<Tag>(root).
 * @return it, or an invalid entity if none matches
 */
template <typename TTerm, typename... TTerms>
Entity findDescendant(const Entity& root)
{
  const DescendantRows<TTerm, TTerms...> rows =
      descendants<TTerm, TTerms...>(root);
  const auto first = rows.begin();
  return first == rows.end() ? Entity{} : Entity((*first).entity());
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

/**
 * One level of a world matrix: `parentWorld` times the local transform's
 * matrix, as affine 3x4 matrices (the columns are the scaled x, y and z axes,
 * then the origin). Every world matrix the engine makes, on demand or cached,
 * is composed from the top down through this one function, so they all agree
 * to the bit.
 *
 * @param parentWorld the parent level's world matrix; null for none (the
 *        local transform's own matrix comes back, with no multiply)
 * @param local the level's local transform
 * @return the level's world matrix
 */
glm::mat4x3 composeWorld(const glm::mat4x3* parentWorld,
                         const TransformComponent& local);

/**
 * A fingerprint of a transform's bytes (position, rotation, scale), which the
 * world cache keeps to notice a transform written since it was made
 * (hashWords() of its five 64-bit words).
 *
 * @param local the transform
 * @return its 64-bit hash
 */
uint64_t transformHash(const TransformComponent& local);

/**
 * Whether a world cache is what composing would give now, given that its
 * parent level is: made (version not 0), under the same nearest ancestor with
 * a transform, and from the same local transform. The transform pass and
 * worldMatrix() both judge a level by it.
 *
 * @param cache the level's WorldTransformComponent
 * @param localHash transformHash() of its TransformComponent now
 * @param holder its nearest ancestor with a transform now (kNone: none)
 * @return whether the cache is current
 */
inline bool cacheCurrent(const WorldTransformComponent& cache,
                         uint64_t localHash,
                         Entity::EntityId holder)
{
  return cache.version != 0 && cache.parent == holder &&
         cache.localHash == localHash;
}

/**
 * Local-to-world matrix: the parents' matrices times the entity's own,
 * composed from the top down (composeWorld()). Always fresh: the levels whose
 * cache (WorldTransformComponent) still matches their transform and parent
 * come from it, and only those below the highest changed level are composed.
 * Identity for a destroyed entity, or one with no transform at or above it.
 */
glm::mat4 worldMatrix(const Entity& entity);

/**
 * The entity's world matrix as this frame's transform pass cached it, for code
 * that runs after the pass in the same frame (the render phase): a read, with
 * no check. An entity whose cache isn't made yet (one created since the last
 * flush) is read through worldMatrix(). Anywhere else, use worldMatrix(): a
 * transform written after the pass isn't seen here until the next pass. With
 * debugFeatures::checkTransforms() on, each read is compared with a fresh one.
 *
 * @param entity the entity, alive
 * @param cache its WorldTransformComponent
 * @param check whether to compare it with a fresh one: what
 *        debugFeatures::checkTransforms() says, which a loop over many
 *        entities reads once and passes
 * @return its world matrix
 */
glm::mat4 cachedWorldMatrix(const Entity& entity,
                            const WorldTransformComponent& cache,
                            bool check = debugFeatures::checkTransforms());

/**
 * cachedWorldMatrix() for an entity whose cache isn't at hand; worldMatrix()
 * for one with no cache.
 */
glm::mat4 cachedWorldMatrix(const Entity& entity);

/**
 * worldTransform() for the render phase: a root's own transform, else its
 * cached world matrix taken apart (see cachedWorldMatrix()). Needs a
 * TransformComponent.
 */
TransformComponent cachedWorldTransform(const Entity& entity);

/**
 * The transform check's test of one cache (see
 * debugFeatures::checkTransforms()): compares its matrix, bit for bit, with
 * the entity's world matrix composed afresh from the top, using no cache. A
 * mismatch is logged once per entity, naming it and `phase`, and the entity
 * is tagged TransformCheckWarned.
 *
 * @param entity the entity
 * @param cache its WorldTransformComponent, made (version not 0)
 * @param phase where the cache was read, for the log ("after the pass")
 * @return whether it matched; true for a destroyed entity, which has nothing
 *         to check
 */
bool checkWorldCache(const Entity& entity,
                     const WorldTransformComponent& cache,
                     const char* phase);

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
