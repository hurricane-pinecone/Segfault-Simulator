#pragma once

class Registry;

class Entity
{
public:
  Entity(int id) : id(id) {};
  ~Entity() = default;

  int getId() const { return id; };

  void setRegistry(Registry* registry) { this->registry = registry; }

  template <typename TComponent, typename... TArgs>
  Entity& addComponent(TArgs&&... args);

  template <typename TComponent>
  void removeComponent() const;

  template <typename TComponent>
  bool hasComponent() const;

  template <typename TComponent>
  TComponent& getComponent() const;

  template <typename TSystem, typename... TArgs>
  void addSystem(TArgs&&... args) const;

  template <typename TSystem>
  void removeSystem() const;

  template <typename TSystem>
  bool hasSystem() const;

  template <typename TSystem>
  TSystem& getSystem() const;

  bool operator==(const Entity& other) const { return id == other.id; }
  bool operator!=(const Entity& other) const { return id != other.id; }
  bool operator>(const Entity& other) const { return id > other.id; }
  bool operator<(const Entity& other) const { return id < other.id; }

private:
  friend class Registry;

  int id;
  Registry* registry;
};
