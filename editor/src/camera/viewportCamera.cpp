#include "camera/viewportCamera.h"

#include "engine/core/rendering/3d/camera3D.h"
#include "glm/glm/common.hpp"
#include "glm/glm/geometric.hpp"

#include <cmath>

namespace
{
constexpr float kOrbitSpeed = 0.006f; // radians per pixel
constexpr float kZoomStep = 0.12f;    // fraction of distance per notch
constexpr float kMinPitch = -1.45f;   // the editor can look from below
constexpr float kMaxPitch = 1.45f;
constexpr float kMinDistance = 0.5f;
constexpr float kMaxDistance = 500.0f;
} // namespace

void ViewportCamera::apply(sfs::Camera3D& camera,
                           const Input& input,
                           float viewportHeight) const
{
  if (input.pan)
  {
    // Move the target so the point under the cursor follows it: scale pixels
    // by the visible world height at the target's distance.
    const float visibleHeight =
        camera.projection == sfs::Camera3D::Projection::Orthographic
            ? camera.orthoHeight
            : 2.0f * camera.distance * std::tan(camera.fieldOfView * 0.5f);
    const float unitsPerPixel = visibleHeight / glm::max(viewportHeight, 1.0f);

    const glm::vec3 forward = camera.viewDirection();
    const glm::vec3 right =
        glm::normalize(glm::cross(forward, glm::vec3(0.0f, 0.0f, 1.0f)));
    const glm::vec3 up = glm::cross(right, forward);

    camera.target += (-right * input.mouseDelta.x + up * input.mouseDelta.y) *
                     unitsPerPixel;
  }
  else if (input.orbit)
  {
    camera.yaw -= input.mouseDelta.x * kOrbitSpeed;
    camera.pitch = glm::clamp(
        camera.pitch + input.mouseDelta.y * kOrbitSpeed, kMinPitch, kMaxPitch);
  }

  if (input.wheel != 0.0f)
  {
    camera.distance = glm::clamp(
        camera.distance * std::pow(1.0f - kZoomStep, input.wheel),
        kMinDistance,
        kMaxDistance);
    camera.orthoHeight = glm::clamp(
        camera.orthoHeight * std::pow(1.0f - kZoomStep, input.wheel),
        kMinDistance,
        kMaxDistance);
  }
}

void ViewportCamera::focus(sfs::Camera3D& camera, const glm::vec3& point) const
{
  camera.target = point;
}
