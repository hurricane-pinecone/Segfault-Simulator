#pragma once

#include <bitset>

const unsigned int MAX_COMPONENTS = 32;
typedef std::bitset<MAX_COMPONENTS> Signature;

class Registry;

class Entity
{
public:
  Entity() : id(-1), registry(nullptr) {};
  Entity(int id) : id(id) {};
  ~Entity() = default;

  int getId() const { return id; };

  bool isValid() const { return id != -1 && registry != nullptr; }

  void setRegistry(Registry* registry) { this->registry = registry; }

  // Templates implemented in registry.h
  // Used for syntactic suger, but still a bit grossed out by this - visit again
  // later maybe
  template <typename TComponent, typename... TArgs>
  Entity& addComponent(TArgs&&... args);

  template <typename TComponent>
  void removeComponent() const;

  template <typename TComponent>
  bool hasComponent() const;

  template <typename TComponent>
  TComponent& getComponent() const;

  bool operator==(const Entity& other) const { return id == other.id; }
  bool operator!=(const Entity& other) const { return id != other.id; }
  bool operator>(const Entity& other) const { return id > other.id; }
  bool operator<(const Entity& other) const { return id < other.id; }

  explicit operator bool() const { return isValid(); }

private:
  friend class Registry;

  int id;
  Registry* registry;
};
