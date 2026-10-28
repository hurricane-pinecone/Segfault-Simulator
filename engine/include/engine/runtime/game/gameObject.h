#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/runtime/input/input.h"
#include "engine/runtime/systems/collision3D.h"

namespace sfs
{

class Scene;
class GameObject
{

public:
  virtual ~GameObject() = default;

  sfs::Entity& entity() { return m_entity; }
  const sfs::Entity& entity() const { return m_entity; }

  /** The scene this object lives in (set before its onCreate runs). */
  Scene& scene() const { return *m_scene; }

protected:
  virtual void onCreate(Scene& scene){};
  virtual void onUpdate(double deltaTime){};
  virtual void onProcessInput(const sfs::Input& input){};

  // Physics (3D scenes): this object's entity began or stopped touching
  // another body, or passing through a trigger (either one being the trigger).
  // Called after the frame's updates.
  virtual void onCollisionEnter(const Collision3D& collision){};
  virtual void onCollisionExit(const Collision3D& collision){};
  virtual void onTriggerEnter(const Collision3D& collision){};
  virtual void onTriggerExit(const Collision3D& collision){};

private:
  friend class Scene;
  void destroy(Scene& scene);

protected:
  sfs::Entity m_entity;

private:
  Scene* m_scene = nullptr;
};

} // namespace sfs
