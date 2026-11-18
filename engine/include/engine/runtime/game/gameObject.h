#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/facilityAccess.h"
#include "engine/core/events/eventOwner.h"
#include "engine/runtime/input/input.h"
#include "engine/runtime/systems/collision2D.h"
#include "engine/runtime/systems/collision3D.h"

namespace sfs
{

class Scene;
class GameObject : public detail::EventUser
{
public:
  ~GameObject() override = default;

  /**
   * Whether it's been destroyed (Scene::destroyObject, or its entity was):
   * it gets no more calls, and is deleted once the frame's updates are done.
   */
  bool destroyed() const { return m_destroyed; }

  sfs::Entity& entity() { return m_entity; }
  const sfs::Entity& entity() const { return m_entity; }

  /** The scene this object lives in (set before its onCreate runs). */
  Scene& scene() const { return *m_scene; }

protected:
  /** The engine's animation clips (facility::Animation). */
  inline facility::Animation animation() const;
  /** The scene's asset loading (facility::Assets). */
  inline facility::Assets assets() const;
  /** The scene's cameras (facility::Cameras). */
  inline facility::Cameras cameras() const;
  /** Debug drawing, in development builds only (facility::DebugDraw). */
  inline facility::DebugDraw debugDraw() const;
  /** The scene's rendering (facility::Graphics). */
  inline facility::Graphics graphics() const;
  /** The scene's meshes (facility::Meshes). */
  inline facility::Meshes meshes() const;
  /** The scene's navigation (facility::Navigation). */
  inline facility::Navigation navigation() const;
  // Physics (3D scenes, PhysicsSystem3D): this object's entity began or
  // stopped touching another body, or passing through a trigger (either one
  // being the trigger). Called as the physics updates, before the objects do.
  virtual void onCollisionEnter(const Collision3D& collision){};
  // The same in a 2D game (PhysicsSystem2D): on the game's plane.
  virtual void onCollisionEnter2D(const Collision2D& collision){};
  virtual void onCollisionExit(const Collision3D& collision){};
  virtual void onCollisionExit2D(const Collision2D& collision){};
  virtual void onCreate(Scene& scene){};
  // Its last call, as it's destroyed: by Scene::destroyObject, when its
  // entity is destroyed, or when its scene ends. Its entity may be gone.
  virtual void onDestroy(){};
  virtual void onProcessInput(const sfs::Input& input){};
  virtual void onTriggerEnter(const Collision3D& collision){};
  virtual void onTriggerEnter2D(const Collision2D& collision){};
  virtual void onTriggerExit(const Collision3D& collision){};
  virtual void onTriggerExit2D(const Collision2D& collision){};
  virtual void onUpdate(double deltaTime){};
  /** The scene's 3D physics (facility::Physics3D). */
  inline facility::Physics3D physics() const;
  /** Prefab copies made as the game runs (facility::Prefabs). */
  inline facility::Prefabs prefabs() const;

  sfs::Entity m_entity;

private:
  friend class Scene;

  void destroy(Scene& scene);

  Scene* m_scene = nullptr;
  bool m_destroyed = false;
  // The owner of the event handles it makes in onCreate
  // (scene().events().consumer<E>(), requester<R>()): the scene runs their
  // handlers before its onUpdate.
  detail::EventOwner m_eventOwner{this};
};

} // namespace sfs
