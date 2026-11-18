#include "engine/runtime/facilities/cameras.h"

#include "absentBackend.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/rendering/camera.h"
#include "engine/runtime/rendering/renderSystem.h"
#include "engine/runtime/systems/cameraSystem.h"

#include <glm/glm/ext/vector_float2.hpp>

#include <atomic>

namespace sfs::facility
{

namespace
{
std::atomic<bool> s_cameraWarned{false};
std::atomic<bool> s_renderWarned{false};
} // namespace

Entity Cameras::active() const
{
  const CameraSystem* cameras = cameraBackend();
  return cameras ? cameras->activeCamera() : Entity{};
}

CameraSystem* Cameras::cameraBackend() const
{
  if (!m_cameras)
    warnAbsent(s_cameraWarned, "cameras", "CameraSystem");

  return m_cameras;
}

Cameras Cameras::in(Registry* registry)
{
  if (!registry)
    return Cameras(nullptr, nullptr);

  return Cameras(registry->tryGetSystem<CameraSystem>(),
                 registry->tryGetSystem<RenderSystem>());
}

RenderSystem* Cameras::renderBackend() const
{
  if (!m_render)
    warnAbsent(s_renderWarned, "cameras", "RenderSystem");

  return m_render;
}

const Camera& Cameras::rendered() const
{
  static const Camera none;
  const RenderSystem* render = renderBackend();
  return render ? render->camera() : none;
}

Ray Cameras::screenRay(const glm::vec2& pixel) const
{
  const RenderSystem* render = renderBackend();
  return render ? render->screenRay(pixel) : Ray{};
}

} // namespace sfs::facility
