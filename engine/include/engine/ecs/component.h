#pragma once

struct BaseComponent
{
protected:
  inline static int nextId = 0;
};

template <typename T>
class Component : public BaseComponent
{
public:
  // Unique id of component T
  // IE: entity1 TransformComponent::getId() -> 1
  //     entity2 TransformComponent::getid() -> 1
  static int getId()
  {
    static int id = nextId++;
    return id;
  }
};
