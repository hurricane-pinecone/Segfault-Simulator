#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/runtime/input/input.h"
#include "engine/runtime/systems/collision2D.h"
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

  /**
   * Whether it's been destroyed (Scene::destroyObject, or its entity was):
   * it gets no more calls, and is deleted once the frame's updates are done.
   */
  bool destroyed() const { return m_destroyed; }

protected:
  virtual void onCreate(Scene& scene){};
  // Its last call, as it's destroyed: by Scene::destroyObject, when its
  // entity is destroyed, or when its scene ends. Its entity may be gone.
  virtual void onDestroy(){};
  virtual void onUpdate(double deltaTime){};
  virtual void onProcessInput(const sfs::Input& input){};

  // Physics (3D scenes, PhysicsSystem3D): this object's entity began or
  // stopped touching another body, or passing through a trigger (either one
  // being the trigger). Called as the physics updates, before the objects do.
  virtual void onCollisionEnter(const Collision3D& collision){};
  virtual void onCollisionExit(const Collision3D& collision){};
  virtual void onTriggerEnter(const Collision3D& collision){};
  virtual void onTriggerExit(const Collision3D& collision){};

  // The same in a 2D game (PhysicsSystem2D): on the game's plane.
  virtual void onCollisionEnter2D(const Collision2D& collision){};
  virtual void onCollisionExit2D(const Collision2D& collision){};
  virtual void onTriggerEnter2D(const Collision2D& collision){};
  virtual void onTriggerExit2D(const Collision2D& collision){};

private:
  friend class Scene;
  void destroy(Scene& scene);

protected:
  sfs::Entity m_entity;

private:
  Scene* m_scene = nullptr;
  bool m_destroyed = false;
};

} // namespace sfs
