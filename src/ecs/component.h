#pragma once

struct BaseComponent
{
protected:
  static int nextId;
};

// Template used to assign unique id per Component<T>
template <typename T>
class Component : public BaseComponent
{
public:
  // Unique id of component T
  static int getId()
  {
    static auto id = nextId++;
    return id;
  }
};
