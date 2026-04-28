#pragma once

struct BaseComponent
{
protected:
  inline static int nextId = 0;
};

// Template used to assign unique id per Component<T>
template <typename T>
class Component : public BaseComponent
{
public:
  // Unique id of component T
  static int getId()
  {
    static int id = nextId++;
    return id;
  }
};
