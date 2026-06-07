#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/runtime/input/input.h"

namespace sfs
{

class Scene;
class GameObject
{

public:
  virtual ~GameObject() = default;

  sfs::Entity& entity() { return m_entity; }
  const sfs::Entity& entity() const { return m_entity; }

protected:
  virtual void onCreate(Scene& scene){};
  virtual void onUpdate(double deltaTime){};
  virtual void onProcessInput(const sfs::Input& input){};

private:
  friend class Scene;
  void destroy(Scene& scene);

protected:
  sfs::Entity m_entity;
};

} // namespace sfs
