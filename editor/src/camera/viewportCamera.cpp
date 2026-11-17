#include "camera/viewportCamera.h"

#include "engine/core/rendering/aabb.h"
#include "engine/core/rendering/camera.h"

#include <glm/glm/common.hpp>
#include <glm/glm/ext/vector_float3.hpp>
#include <glm/glm/geometric.hpp>
#include <glm/glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace
{
constexpr float ORBIT_SPEED = 0.006f; // radians per pixel
constexpr float ZOOM_STEP = 0.12f;    // fraction of distance per notch
constexpr float MIN_PITCH = -1.45f;   // the editor can look from below
constexpr float MAX_PITCH = 1.45f;
constexpr float MIN_DISTANCE = 0.5f;
constexpr float MAX_DISTANCE = 500.0f;
// A 2D game's world is in its pixels (or tiles): a far wider zoom.
constexpr float MIN_PLANAR_HEIGHT = 1.0f;
constexpr float MAX_PLANAR_HEIGHT = 100000.0f;
// Far enough above the plane to see everything on it (layers, iso depth).
constexpr float PLANAR_DISTANCE = 50000.0f;
} // namespace

void ViewportCamera::apply(sfs::Camera& camera,
                           const Input& input,
                           float viewportHeight) const
{
  if (input.pan || (input.planar && input.orbit))
  {
    // Move the target so the point under the cursor follows it: scale pixels
    // by the visible world height at the target's distance.
    const float visibleHeight =
        camera.projection == sfs::Camera::Projection::Orthographic
            ? camera.orthoHeight
            : 2.0f * camera.distance * std::tan(camera.fieldOfView * 0.5f);
    const float unitsPerPixel = visibleHeight / glm::max(viewportHeight, 1.0f);

    // Across the view and up it (as the view matrix has them, so it holds
    // looking straight down too).
    const glm::vec3 forward = camera.viewDirection();
    const glm::vec3 right = camera.groundRight();
    const glm::vec3 up = glm::cross(right, forward);

    camera.target +=
        (-right * input.mouseDelta.x + up * input.mouseDelta.y) * unitsPerPixel;
  }
  else if (input.orbit)
  {
    camera.yaw -= input.mouseDelta.x * ORBIT_SPEED;
    camera.pitch = glm::clamp(
        camera.pitch + input.mouseDelta.y * ORBIT_SPEED, MIN_PITCH, MAX_PITCH);
  }

  if (input.wheel != 0.0f && input.planar)
    camera.orthoHeight =
        glm::clamp(camera.orthoHeight * std::pow(1.0f - ZOOM_STEP, input.wheel),
                   MIN_PLANAR_HEIGHT,
                   MAX_PLANAR_HEIGHT);
  else if (input.wheel != 0.0f)
  {
    camera.distance =
        glm::clamp(camera.distance * std::pow(1.0f - ZOOM_STEP, input.wheel),
                   MIN_DISTANCE,
                   MAX_DISTANCE);
    camera.orthoHeight =
        glm::clamp(camera.orthoHeight * std::pow(1.0f - ZOOM_STEP, input.wheel),
                   MIN_DISTANCE,
                   MAX_DISTANCE);
  }
}

void ViewportCamera::focus(sfs::Camera& camera, const sfs::Aabb& bounds) const
{
  if (bounds.empty())
    return;

  // Fit a sphere round the box into the view, with some room around it.
  constexpr float MARGIN = 1.3f;
  const float radius = std::max(glm::length(bounds.extents()), 0.25f);
  camera.target = bounds.center();

  // A 2D game's view stays where it is above the plane; only its zoom fits.
  if (camera.distance >= PLANAR_DISTANCE)
  {
    camera.orthoHeight = glm::clamp(
        radius * 2.0f * MARGIN, MIN_PLANAR_HEIGHT, MAX_PLANAR_HEIGHT);
    return;
  }

  camera.distance =
      glm::clamp(radius * MARGIN / std::sin(camera.fieldOfView * 0.5f),
                 MIN_DISTANCE,
                 MAX_DISTANCE);
  camera.orthoHeight =
      glm::clamp(radius * 2.0f * MARGIN, MIN_DISTANCE, MAX_DISTANCE);
}

void ViewportCamera::makeAngled(sfs::Camera& camera)
{
  const sfs::Camera defaults;
  camera.projection = sfs::Camera::Projection::Perspective;
  camera.yaw = defaults.yaw;
  camera.pitch = defaults.pitch;
  camera.distance = glm::clamp(camera.distance, MIN_DISTANCE, 20.0f);
  camera.nearPlane = defaults.nearPlane;
  camera.farPlane = defaults.farPlane;
  camera.orthoHeight = glm::clamp(camera.orthoHeight, MIN_DISTANCE, 20.0f);
}

void ViewportCamera::makePlanar(sfs::Camera& camera)
{
  camera.projection = sfs::Camera::Projection::Orthographic;
  camera.yaw = glm::half_pi<float>(); // the plane's +y up the screen
  camera.pitch = glm::half_pi<float>();
  camera.distance = PLANAR_DISTANCE;
  camera.nearPlane = 1.0f;
  camera.farPlane = PLANAR_DISTANCE * 2.0f;
  camera.orthoHeight =
      glm::clamp(camera.orthoHeight, MIN_PLANAR_HEIGHT, MAX_PLANAR_HEIGHT);
}
