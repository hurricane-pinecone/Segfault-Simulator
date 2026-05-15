#pragma once

#include "engine/ecs/entity.h"
#include "engine/input/input.h"

namespace sfs
{

class Scene;
class GameObject
{

public:
  virtual ~GameObject() = default;

  virtual void onCreate(Scene& scene) {};
  virtual void onUpdate(double deltaTime) {};
  virtual void onProcessInput(const sfs::Input& input) {};

  sfs::Entity& entity() { return m_entity; }
  const sfs::Entity& entity() const { return m_entity; }

protected:
  sfs::Entity m_entity;
};

} // namespace sfs
