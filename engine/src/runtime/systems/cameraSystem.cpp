#include "engine/runtime/systems/cameraSystem.h"

#include "engine/core/components/boxCollider3DComponent.h"
#include "engine/core/components/capsuleCollider3DComponent.h"
#include "engine/core/components/rigidBody3DComponent.h"
#include "engine/core/components/sphereCollider3DComponent.h"
#include "engine/core/rendering/camera.h"
#include "glm/glm/geometric.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sfs
{

namespace
{
// A ray straight down from above everything it is tested against.
constexpr glm::vec3 kDown{0.0f, 0.0f, -1.0f};

glm::vec3 scaleOf(const glm::mat4& world)
{
  return {glm::length(glm::vec3(world[0])),
          glm::length(glm::vec3(world[1])),
          glm::length(glm::vec3(world[2]))};
}

// Whether a collider counts for these settings: solid, on a blocking layer.
bool counts(const CameraCollisionComponent& collision, bool trigger, int layer)
{
  if (trigger || layer < 0 || layer > 31)
    return false;
  return (static_cast<unsigned>(collision.layers) >> layer) & 1u;
}

// Where a ray first meets a sphere, or a negative distance for a miss.
float hitSphere(const glm::vec3& origin,
                const glm::vec3& direction,
                const glm::vec3& centre,
                float radius)
{
  const glm::vec3 oc = origin - centre;
  const float b = glm::dot(direction, oc);
  const float h = b * b - (glm::dot(oc, oc) - radius * radius);
  return h < 0.0f ? -1.0f : -b - std::sqrt(h);
}

// Where a ray (unit direction) first meets a capsule round the segment a-b,
// or a negative distance for a miss (Inigo Quilez's capsule intersection).
float hitCapsule(const glm::vec3& origin,
                 const glm::vec3& direction,
                 const glm::vec3& a,
                 const glm::vec3& b,
                 float radius)
{
  const glm::vec3 ba = b - a, oa = origin - a;
  const float baba = glm::dot(ba, ba), bard = glm::dot(ba, direction),
              baoa = glm::dot(ba, oa);
  const float k = baba - bard * bard;
  // Along its axis (or no axis at all): only its ends can be met first.
  if (k < 1.0e-8f * std::max(baba, 1.0f))
  {
    const float ta = hitSphere(origin, direction, a, radius),
                tb = hitSphere(origin, direction, b, radius);
    if (ta < 0.0f)
      return tb;
    return tb < 0.0f ? ta : std::min(ta, tb);
  }
  const float rdoa = glm::dot(direction, oa), oaoa = glm::dot(oa, oa);
  const float qb = baba * rdoa - baoa * bard;
  const float qc = baba * oaoa - baoa * baoa - radius * radius * baba;
  const float h = qb * qb - k * qc;
  if (h < 0.0f)
    return -1.0f;
  const float t = (-qb - std::sqrt(h)) / k;
  const float y = baoa + t * bard;
  if (y > 0.0f && y < baba)
    return t;
  return hitSphere(origin, direction, y <= 0.0f ? a : b, radius);
}
} // namespace

void CameraSystem::gatherBlockers(const CameraCollisionComponent& collision,
                                  const glm::vec3& pivot,
                                  float reach)
{
  m_blockers.clear();
  const auto keep = [&](Blocker&& blocker)
  {
    const Aabb& box = blocker.bounds;
    if (box.empty() || box.max.z - box.min.z < collision.minHeight ||
        box.max.x < pivot.x - reach || box.min.x > pivot.x + reach ||
        box.max.y < pivot.y - reach || box.min.y > pivot.y + reach)
      return;
    m_blockers.push_back(std::move(blocker));
  };
  const auto still = [&](const Entity& entity)
  {
    return collision.movingBlocks ||
           !entity.hasComponent<RigidBody3DComponent>();
  };

  for (auto [entity, transform, box] :
       registry->query<TransformComponent, BoxCollider3DComponent>())
  {
    if (!counts(collision, box.trigger, box.layer) || !still(entity))
      continue;
    const glm::mat4 world = hierarchy::worldMatrix(entity);
    Blocker blocker;
    blocker.kind = Blocker::Kind::Box;
    blocker.box.expand(box.offset - box.size * 0.5f);
    blocker.box.expand(box.offset + box.size * 0.5f);
    blocker.bounds = blocker.box.transformed(world);
    blocker.toLocal = glm::inverse(world);
    keep(std::move(blocker));
  }
  for (auto [entity, transform, sphere] :
       registry->query<TransformComponent, SphereCollider3DComponent>())
  {
    if (!counts(collision, sphere.trigger, sphere.layer) || !still(entity))
      continue;
    const glm::mat4 world = hierarchy::worldMatrix(entity);
    const glm::vec3 s = scaleOf(world);
    Blocker blocker;
    blocker.kind = Blocker::Kind::Sphere;
    blocker.a = glm::vec3(world * glm::vec4(sphere.offset, 1.0f));
    blocker.radius = sphere.radius * std::max({s.x, s.y, s.z});
    blocker.bounds.expand(blocker.a - glm::vec3(blocker.radius));
    blocker.bounds.expand(blocker.a + glm::vec3(blocker.radius));
    keep(std::move(blocker));
  }
  for (auto [entity, transform, capsule] :
       registry->query<TransformComponent, CapsuleCollider3DComponent>())
  {
    if (!counts(collision, capsule.trigger, capsule.layer) || !still(entity))
      continue;
    const glm::mat4 world = hierarchy::worldMatrix(entity);
    const glm::vec3 s = scaleOf(world);
    Blocker blocker;
    blocker.kind = Blocker::Kind::Capsule;
    blocker.radius = capsule.radius * std::max(s.x, s.y);
    // Upright along the entity's own Z.
    const float half =
        std::max(capsule.height * s.z * 0.5f - blocker.radius, 0.0f);
    const glm::vec3 centre(world * glm::vec4(capsule.offset, 1.0f));
    const glm::vec3 up =
        s.z > 0.0f ? glm::vec3(world[2]) / s.z : glm::vec3(0.0f, 0.0f, 1.0f);
    blocker.a = centre - up * half;
    blocker.b = centre + up * half;
    const glm::vec3 round(blocker.radius);
    blocker.bounds.expand(glm::min(blocker.a, blocker.b) - round);
    blocker.bounds.expand(glm::max(blocker.a, blocker.b) + round);
    keep(std::move(blocker));
  }
}

bool CameraSystem::topAt(const glm::vec2& point, float& top) const
{
  bool found = false;
  for (const Blocker& blocker : m_blockers)
  {
    const Aabb& box = blocker.bounds;
    if (point.x < box.min.x || point.x > box.max.x || point.y < box.min.y ||
        point.y > box.max.y || (found && box.max.z <= top))
      continue;
    const glm::vec3 origin(point, box.max.z + 1.0f);
    float distance = -1.0f;
    switch (blocker.kind)
    {
    case Blocker::Kind::Box:
    {
      // In its own space the ray's parameter still counts world units.
      Ray local;
      local.origin = glm::vec3(blocker.toLocal * glm::vec4(origin, 1.0f));
      local.direction = glm::vec3(blocker.toLocal * glm::vec4(kDown, 0.0f));
      if (!local.intersectAabb(blocker.box, distance))
        distance = -1.0f;
      break;
    }
    case Blocker::Kind::Sphere:
      distance = hitSphere(origin, kDown, blocker.a, blocker.radius);
      break;
    case Blocker::Kind::Capsule:
      distance =
          hitCapsule(origin, kDown, blocker.a, blocker.b, blocker.radius);
      break;
    }
    if (distance < 0.0f)
      continue;
    const float z = origin.z - distance;
    if (!found || z > top)
      top = z;
    found = true;
  }
  return found;
}

void CameraSystem::lateUpdate(double deltaTime)
{
  if (!registry)
    return;
  const float dt = static_cast<float>(deltaTime);
  for (auto [entity, transform, camera, collision] :
       registry->query<TransformComponent,
                       CameraComponent,
                       CameraCollisionComponent>())
  {
    const float focus = std::max(camera.focusDistance, 0.01f);

    // Left where it was lifted to: nothing moved it, so the controller's pose
    // is still the one it was lifted from.
    if (collision.wrote && transform.position == collision.writtenPosition &&
        transform.rotation == collision.writtenRotation)
    {
      transform.position = collision.fromPosition;
      transform.rotation = collision.fromRotation;
    }
    collision.fromPosition = transform.position;
    collision.fromRotation = transform.rotation;

    // Turned off: no lift, and what there was eases back as when clear.
    if (!collision.enabled)
    {
      collision.lift *= std::exp(-collision.easeBack * dt);
      collision.wrote = false;
      continue;
    }

    // The controller's view, as an orbit about the pivot.
    TransformComponent world = hierarchy::worldTransform(entity);
    Camera view;
    view.setPose(
        world.position, world.rotation * glm::vec3(1.0f, 0.0f, 0.0f), focus);
    const float maxPitch = glm::radians(collision.maxPitch);

    // Each point along the ground from the pivot back to under the eye wants
    // the line to pass over what stands there, by more toward the eye; the
    // steepest it asks for is the pitch. Raising the pitch draws the eye in
    // over the ground, so it's worked out again from there.
    gatherBlockers(collision, view.target, focus + 2.0f);
    const glm::vec2 back = -glm::vec2(view.groundForward());
    const glm::vec2 pivot(view.target);
    const int samples = std::max(collision.samples, 1);
    float pitch = view.pitch;
    if (!m_blockers.empty())
      for (int pass = 0; pass < 2; ++pass)
      {
        const float run = focus * std::cos(pitch);
        float steepest = view.pitch;
        for (int i = 1; i <= samples; ++i)
        {
          const float along = static_cast<float>(i) / samples;
          const float out = run * along;
          float top = 0.0f;
          if (out < 1.0f || !topAt(pivot + back * out, top))
            continue;
          const float clear =
              top + collision.targetClearance +
              (collision.eyeClearance - collision.targetClearance) * along;
          steepest = std::max(steepest, std::atan2(clear - view.target.z, out));
        }
        pitch = std::min(steepest, maxPitch);
      }

    // Up at once, easing back down once clear.
    const float wanted = std::max(pitch - view.pitch, 0.0f);
    collision.lift =
        wanted > collision.lift
            ? wanted
            : collision.lift + (wanted - collision.lift) *
                                   (1.0f - std::exp(-collision.easeBack * dt));
    const float shown = std::min(view.pitch + collision.lift, maxPitch);
    if (shown > view.pitch)
    {
      view.pitch = shown;
      world.position = view.position();
      world.rotation = view.orientation();
      hierarchy::setWorldMatrix(entity, world.matrix());
    }
    collision.writtenPosition = transform.position;
    collision.writtenRotation = transform.rotation;
    collision.wrote = true;
  }
}

} // namespace sfs
