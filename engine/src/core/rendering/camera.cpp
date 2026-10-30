#include "engine/core/rendering/camera.h"

#include "glm/glm/ext/matrix_clip_space.hpp"
#include "glm/glm/ext/matrix_transform.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/gtc/quaternion.hpp"
#include "glm/glm/matrix.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace sfs
{

namespace
{
constexpr glm::vec3 kUp{0.0f, 0.0f, 1.0f};
}

bool Ray::intersectPlaneZ(float height, float& outDistance) const
{
  if (std::abs(direction.z) < 1.0e-6f)
    return false;

  const float t = (height - origin.z) / direction.z;
  if (t < 0.0f)
    return false;

  outDistance = t;
  return true;
}

bool Ray::intersectAabb(const Aabb& box, float& outDistance) const
{
  if (box.empty())
    return false;

  // Slab method: clip the ray against each axis' pair of planes.
  float near = 0.0f;
  float far = std::numeric_limits<float>::max();
  for (int axis = 0; axis < 3; ++axis)
  {
    if (std::abs(direction[axis]) < 1.0e-8f)
    {
      // Parallel to this slab: inside it or a miss.
      if (origin[axis] < box.min[axis] || origin[axis] > box.max[axis])
        return false;
      continue;
    }

    const float inverse = 1.0f / direction[axis];
    float t0 = (box.min[axis] - origin[axis]) * inverse;
    float t1 = (box.max[axis] - origin[axis]) * inverse;
    if (t0 > t1)
      std::swap(t0, t1);
    near = std::max(near, t0);
    far = std::min(far, t1);
    if (near > far)
      return false;
  }

  outDistance = near;
  return true;
}

glm::vec3 Camera::position() const
{
  return target - viewDirection() * distance;
}

glm::vec3 Camera::viewDirection() const
{
  // Looking from the eye toward the target: the eye sits behind (-forward) and
  // above, so the view tilts down by `pitch`.
  const float c = std::cos(pitch);
  return glm::vec3(std::cos(yaw) * c, std::sin(yaw) * c, -std::sin(pitch));
}

glm::vec3 Camera::groundForward() const
{
  return glm::vec3(std::cos(yaw), std::sin(yaw), 0.0f);
}

glm::vec3 Camera::groundRight() const
{
  return glm::vec3(std::sin(yaw), -std::cos(yaw), 0.0f);
}

void Camera::setPose(const glm::vec3& eye,
                     const glm::vec3& forward,
                     float focusDistance,
                     const glm::vec3& up)
{
  const float length = glm::length(forward);
  if (length < 1.0e-6f)
    return;
  const glm::vec3 direction = forward / length;
  // Looking straight up or down, the heading is which way is up the screen.
  const bool vertical =
      direction.x * direction.x + direction.y * direction.y < 1.0e-8f;
  const glm::vec3 heading = vertical ? up * -direction.z : direction;
  yaw = std::atan2(heading.y, heading.x);
  pitch = std::asin(std::clamp(-direction.z, -1.0f, 1.0f));
  distance = std::max(focusDistance, 1.0e-3f);
  target = eye + direction * distance;
}

glm::quat Camera::orientation() const
{
  // Turn to face yaw around Z, then tip the nose down by pitch (a positive
  // turn about the turned Y axis takes +X toward -Z).
  return glm::angleAxis(yaw, kUp) *
         glm::angleAxis(pitch, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 Camera::viewMatrix() const
{
  // Up the screen: at right angles to the view and its ground-level right,
  // so a camera looking straight down still knows which way is up.
  return glm::lookAtRH(
      position(), target, glm::cross(groundRight(), viewDirection()));
}

glm::mat4 Camera::projectionMatrix(float aspect) const
{
  if (projection == Projection::Orthographic)
  {
    const float halfHeight = orthoHeight * 0.5f;
    const float halfWidth = halfHeight * aspect;
    return glm::orthoRH_ZO(
        -halfWidth, halfWidth, -halfHeight, halfHeight, nearPlane, farPlane);
  }

  return glm::perspectiveRH_ZO(fieldOfView, aspect, nearPlane, farPlane);
}

glm::mat4 Camera::viewProjection(float aspect) const
{
  return projectionMatrix(aspect) * viewMatrix();
}

Ray Camera::screenRay(const glm::vec2& pixel,
                      const glm::vec2& viewportSize) const
{
  const float aspect = viewportSize.x / viewportSize.y;
  const glm::mat4 inverse = glm::inverse(viewProjection(aspect));

  // Pixel (Y down) -> NDC (Y up); unproject the near and far plane points.
  const glm::vec2 ndc{pixel.x / viewportSize.x * 2.0f - 1.0f,
                      1.0f - pixel.y / viewportSize.y * 2.0f};

  const glm::vec4 nearPoint = inverse * glm::vec4(ndc, 0.0f, 1.0f);
  const glm::vec4 farPoint = inverse * glm::vec4(ndc, 1.0f, 1.0f);
  const glm::vec3 nearWorld = glm::vec3(nearPoint) / nearPoint.w;
  const glm::vec3 farWorld = glm::vec3(farPoint) / farPoint.w;

  return Ray{nearWorld, glm::normalize(farWorld - nearWorld)};
}

} // namespace sfs
