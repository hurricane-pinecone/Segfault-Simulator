#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/facilityAccess.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/ecs/service.h"
#include "engine/core/ecs/system.h"
#include "engine/core/rendering/camera.h"
#include "engine/runtime/game/gameObject.h"
#include "engine/runtime/sceneManager/scene.h"

#include <glm/glm/ext/vector_float2.hpp>

namespace sfs
{

class CameraSystem;
class RenderSystem;

namespace facility
{

/**
 * The scene's cameras, as game code uses it (`cameras()` on a System,
 * Service, GameObject or Scene): which camera entity is active, the camera
 * as last drawn, and the ray through a pixel of the view. A value handle
 * over the scene's CameraSystem and RenderSystem, valid for the step that
 * got it: get it where it's used and keep it in no member. Without them its
 * queries give empty answers, with one warning a run (available() tells).
 */
class Cameras
{
public:
  /**
   * The scene's active camera entity (its first with an active
   * CameraComponent and a TransformComponent).
   * @return it; invalid with none, or no CameraSystem
   */
  Entity active() const;

  /**
   * @return whether the scene has both a CameraSystem (for active()) and a
   *         RenderSystem (for rendered() and screenRay()); each of those
   *         answers with the one it needs
   */
  inline bool available() const { return m_cameras && m_render; }

  /**
   * The cameras of a registry's scene, as the accessors give it.
   * @param registry the registry (null: none)
   * @return its facade, absent where it has no CameraSystem or RenderSystem
   */
  static Cameras in(Registry* registry);

  /**
   * The camera as last drawn: where the eye was, which way it looked, its
   * projection (a lamp's glow toward the eye).
   * @return the camera; a default one when the scene doesn't render
   */
  const Camera& rendered() const;

  /**
   * The world-space ray through a pixel of the scene's view (the mouse
   * position is in the same pixels), from the camera as last drawn.
   * @param pixel the pixel
   * @return the ray; a default one when the scene doesn't render
   */
  Ray screenRay(const glm::vec2& pixel) const;

private:
  inline Cameras(CameraSystem* cameras, RenderSystem* render)
      : m_cameras(cameras), m_render(render)
  {
  }

  // The parts behind it; null (and each warned about once) when the scene
  // has none.
  CameraSystem* cameraBackend() const;
  RenderSystem* renderBackend() const;

  CameraSystem* m_cameras = nullptr;
  RenderSystem* m_render = nullptr;
};

} // namespace facility

inline facility::Cameras System::cameras() const
{
  return facility::Cameras::in(registry);
}

inline facility::Cameras Service::cameras() const
{
  return facility::Cameras::in(m_registry);
}

inline facility::Cameras GameObject::cameras() const
{
  return m_scene ? m_scene->cameras() : facility::Cameras::in(nullptr);
}

inline facility::Cameras Scene::cameras()
{
  return facility::Cameras::in(&m_registry);
}

} // namespace sfs
