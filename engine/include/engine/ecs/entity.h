#pragma once

#include <bitset>
#include <cstdint>

namespace sfs
{

const unsigned int MAX_COMPONENTS = 32;
typedef std::bitset<MAX_COMPONENTS> Signature;

class Registry;

class Entity
{
public:
  using EntityId = uint32_t;
  using DebugId = uint64_t;

  static constexpr EntityId InvalidId = std::numeric_limits<EntityId>::max();
  static constexpr DebugId InvalidDebugId = std::numeric_limits<DebugId>::max();

  Entity() : id(InvalidId), debugId(InvalidDebugId), generation(0) {};
  Entity(EntityId id, uint32_t generation, DebugId debugId)
      : id(id), debugId(debugId), generation(generation)
  {
  }
  ~Entity() = default;

  EntityId getId() const { return id; };
  DebugId getDebugId() const { return debugId; }
  uint32_t getGeneration() const { return generation; }
  bool isAlive() const;
  bool isValid() const { return id != InvalidId && registry != nullptr; }

  void setRegistry(Registry* registry) { this->registry = registry; }

  template <typename TComponent, typename... TArgs>
  Entity& addComponent(TArgs&&... args);

  // This is exactly the same as addComponent, but it's nicer semantically
  // for game client
  template <typename TComponent, typename... TArgs>
  Entity& addTag(TArgs&&... args);

  template <typename TComponent>
  void removeComponent() const;

  template <typename TComponent>
  bool hasComponent() const;

  template <typename TComponent>
  TComponent& getComponent() const;

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

  explicit operator bool() const { return isValid(); }

private:
  friend class Registry;

  EntityId id;     // Id is reused when entities are destroyed, because ECS uses
                   // vector storage. This is to avoid infinite memory growth.
  DebugId debugId; // Remains consistent.
  uint32_t generation;
  Registry* registry = nullptr;
};

} // namespace sfs
