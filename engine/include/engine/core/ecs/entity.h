#pragma once

#include <bitset>
#include <cstdint>
#include <limits>

namespace sfs
{

// Width of the per-entity component Signature bitset, and therefore the maximum
// number of distinct component types the ECS can register. Each component type
// claims one id (Component<T>::getId), used as a bit index here; registering
// more types than this is a hard misconfiguration (see component.h guard).
const unsigned int MAX_COMPONENTS = 256;
typedef std::bitset<MAX_COMPONENTS> Signature;

class Registry;

class Entity
{
public:
  using EntityId = uint32_t;
  using DebugId = uint64_t;

  static constexpr EntityId InvalidId = std::numeric_limits<EntityId>::max();
  static constexpr DebugId InvalidDebugId = std::numeric_limits<DebugId>::max();

  Entity() : id(InvalidId), generation(0), debugId(InvalidDebugId){};
  Entity(EntityId id, uint32_t generation, DebugId debugId)
      : id(id), generation(generation), debugId(debugId)
  {
  }
  ~Entity() = default;

  EntityId getId() const { return id; };
  DebugId getDebugId() const { return debugId; }
  uint32_t getGeneration() const { return generation; }
  /**
   * Whether it refers to a living entity: false for a default-made handle,
   * and once its entity is destroyed (at the flush), even after a new entity
   * reuses the id. One inline check (Registry::isValid()). A handle must not
   * outlive its registry: this reads it.
   * @return whether the entity lives
   */
  bool isValid() const;

  void setRegistry(Registry* registry) { this->registry = registry; }
  /** The registry the entity belongs to; null for a default-made handle. */
  Registry* registryOf() const { return registry; }

  /**
   * Gives the entity a TComponent made from `args`, replacing one it has. On
   * a handle that isn't valid it logs an error and adds nothing.
   * @return this handle, so calls chain
   */
  template <typename TComponent, typename... TArgs>
  Entity& addComponent(TArgs&&... args);

  /** addComponent(), for a tag (an empty component): it reads better. */
  template <typename TComponent, typename... TArgs>
  Entity& addTag(TArgs&&... args);

  /** Takes its TComponent away; nothing happens if it has none. */
  template <typename TComponent>
  void removeComponent() const;

  /** Whether it's a living entity with a TComponent. */
  template <typename TComponent>
  bool hasComponent() const;

  /**
   * Whether it's a living entity whose components match a query's terms, as
   * Registry::query() reads them: components it must all have, one Any<...>
   * group it must have one of, Excluding<...> groups it must have none of,
   * and Query<...> lists of these, e.g. `e.matches<Served>()` for a
   * `using Served = sfs::Query<...>`. It tests its components now, as
   * hasComponent() does, so an entity not yet flushed into the query's list
   * can already match, and a destroyed one still matches until its flush
   * removes it. One signature test, no allocation.
   * @return false for a handle that isn't valid
   */
  template <typename... TTerms>
  bool matches() const;

  /**
   * Its TComponent, in one inline lookup.
   * @return the component, or null when it has none or the handle isn't
   *         valid
   */
  template <typename TComponent>
  TComponent* getComponent() const;

  bool operator==(const Entity& other) const
  {
    return id == other.id && generation == other.generation;
  }
  bool operator!=(const Entity& other) const { return !(*this == other); }
  bool operator>(const Entity& other) const
  {
    if (id != other.id)
      return id > other.id;
    return generation > other.generation;
  }
  bool operator<(const Entity& other) const
  {
    if (id != other.id)
      return id < other.id;

    return generation < other.generation;
  }

  /** isValid(): whether it refers to a living entity. */
  explicit operator bool() const { return isValid(); }

private:
  friend class Registry;

  EntityId id;         // Id is reused when entities are destroyed, because ECS
                       // uses vector storage. This is to avoid infinite memory
                       // growth.
  uint32_t generation; // Which use of the id it is.
  DebugId debugId;     // Remains consistent.
  Registry* registry = nullptr;
};

} // namespace sfs
