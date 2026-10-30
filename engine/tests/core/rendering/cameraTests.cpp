#include "../../testHarness.h"

#include <engine/core/rendering/camera.h>

#include <glm/glm/ext/scalar_constants.hpp>
#include <glm/glm/geometric.hpp>
#include <glm/glm/gtc/quaternion.hpp>

using namespace sfs;

namespace
{
bool nearVec(const glm::vec3& a, const glm::vec3& b, float eps = 1e-3f)
{
  return testing::approx(a.x, b.x, eps) && testing::approx(a.y, b.y, eps) &&
         testing::approx(a.z, b.z, eps);
}

// Clip-space position after the perspective divide.
glm::vec3 project(const glm::mat4& viewProjection, const glm::vec3& world)
{
  const glm::vec4 clip = viewProjection * glm::vec4(world, 1.0f);
  return glm::vec3(clip) / clip.w;
}
} // namespace

int main()
{
  TEST("the eye should sit distance away from the target")
  {
    Camera camera;
    camera.target = {5.0f, -2.0f, 1.0f};
    camera.distance = 12.0f;
    camera.yaw = 1.1f;
    camera.pitch = 0.7f;
    CHECK(testing::approx(
        glm::length(camera.position() - camera.target), 12.0f, 1e-3f));
  }

  TEST("a positive pitch should raise the eye above the target")
  {
    Camera camera;
    camera.pitch = 0.5f;
    CHECK(camera.position().z > camera.target.z);
    CHECK(camera.viewDirection().z < 0.0f);
  }

  TEST("yaw 0 should look along +X")
  {
    Camera camera;
    camera.yaw = 0.0f;
    CHECK(nearVec(camera.groundForward(), {1.0f, 0.0f, 0.0f}));
    CHECK(nearVec(camera.groundRight(), {0.0f, -1.0f, 0.0f}));
  }

  TEST("ground axes should be horizontal, unit and perpendicular")
  {
    Camera camera;
    camera.yaw = 2.3f;
    const glm::vec3 forward = camera.groundForward();
    const glm::vec3 right = camera.groundRight();
    CHECK(testing::approx(forward.z, 0.0f));
    CHECK(testing::approx(glm::length(forward), 1.0f));
    CHECK(testing::approx(glm::dot(forward, right), 0.0f));
    // Right-handed with Z up: forward x right points down.
    CHECK(glm::cross(forward, right).z < 0.0f);
  }

  TEST("the target should project to the centre of the screen")
  {
    Camera camera;
    camera.target = {3.0f, 4.0f, 0.0f};
    camera.yaw = 0.4f;
    const glm::vec3 ndc =
        project(camera.viewProjection(16.0f / 9.0f), camera.target);
    CHECK(testing::approx(ndc.x, 0.0f, 1e-4f));
    CHECK(testing::approx(ndc.y, 0.0f, 1e-4f));
    CHECK(ndc.z > 0.0f);
    CHECK(ndc.z < 1.0f);
  }

  TEST("depth should map near to 0 and far to 1")
  {
    Camera camera;
    camera.nearPlane = 1.0f;
    camera.farPlane = 100.0f;
    const glm::mat4 vp = camera.viewProjection(1.0f);
    const glm::vec3 eye = camera.position();
    const glm::vec3 dir = camera.viewDirection();
    CHECK(testing::approx(project(vp, eye + dir * 1.0f).z, 0.0f, 1e-4f));
    CHECK(testing::approx(project(vp, eye + dir * 100.0f).z, 1.0f, 1e-4f));
  }

  TEST("orthographic should also centre the target")
  {
    Camera camera;
    camera.projection = Camera::Projection::Orthographic;
    camera.target = {-7.0f, 2.0f, 3.0f};
    const glm::vec3 ndc = project(camera.viewProjection(1.5f), camera.target);
    CHECK(testing::approx(ndc.x, 0.0f, 1e-4f));
    CHECK(testing::approx(ndc.y, 0.0f, 1e-4f));
  }

  TEST("the screen-centre ray should pass through the target")
  {
    Camera camera;
    camera.target = {2.0f, 2.0f, 0.0f};
    camera.yaw = 0.9f;
    camera.pitch = 0.8f;
    const Ray ray = camera.screenRay({640.0f, 360.0f}, {1280.0f, 720.0f});
    float distance = 0.0f;
    CHECK(ray.intersectPlaneZ(0.0f, distance));
    CHECK(nearVec(ray.at(distance), camera.target, 1e-2f));
  }

  TEST("a ray from above should not hit a plane above the eye")
  {
    Ray ray{{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}};
    float distance = 0.0f;
    CHECK(!ray.intersectPlaneZ(10.0f, distance));
    CHECK(ray.intersectPlaneZ(1.0f, distance));
    CHECK(testing::approx(distance, 4.0f));
  }

  TEST("a ray should hit a box in front and report the entry distance")
  {
    Aabb box;
    box.expand(glm::vec3(-1.0f, -1.0f, -1.0f));
    box.expand(glm::vec3(1.0f, 1.0f, 1.0f));
    float distance = 0.0f;

    Ray hit{{-5.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};
    CHECK(hit.intersectAabb(box, distance));
    CHECK(testing::approx(distance, 4.0f));

    Ray inside{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    CHECK(inside.intersectAabb(box, distance));
    CHECK(testing::approx(distance, 0.0f));
  }

  TEST("a ray should miss boxes beside or behind it")
  {
    Aabb box;
    box.expand(glm::vec3(-1.0f));
    box.expand(glm::vec3(1.0f));
    float distance = 0.0f;

    CHECK(!Ray({{-5.0f, 3.0f, 0.0f}, {1.0f, 0.0f, 0.0f}})
               .intersectAabb(box, distance));
    CHECK(!Ray({{5.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}})
               .intersectAabb(box, distance));
    CHECK(!Ray({{0.0f, 5.0f, 0.0f}, {1.0f, 0.0f, 0.0f}})
               .intersectAabb(box, distance));
    CHECK(!Ray({{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}})
               .intersectAabb(Aabb{}, distance));
  }

  TEST("setPose should look from the eye along the direction")
  {
    Camera camera;
    const glm::vec3 eye{3.0f, -4.0f, 6.0f};
    const glm::vec3 forward = glm::normalize(glm::vec3{-1.0f, 2.0f, -1.5f});
    camera.setPose(eye, forward * 7.0f, 12.0f);

    CHECK(nearVec(camera.position(), eye));
    CHECK(nearVec(camera.viewDirection(), forward));
    CHECK(testing::approx(camera.distance, 12.0f));
    CHECK(nearVec(camera.target, eye + forward * 12.0f));
  }

  TEST("orientation should turn +X onto the view direction and keep +Z up")
  {
    Camera camera;
    camera.yaw = 2.1f;
    camera.pitch = 0.7f;
    const glm::quat orientation = camera.orientation();
    CHECK(nearVec(
        orientation * glm::vec3(1.0f, 0.0f, 0.0f), camera.viewDirection()));
    CHECK((orientation * glm::vec3(0.0f, 0.0f, 1.0f)).z > 0.0f);
    // Level: the side axis stays horizontal.
    CHECK(testing::approx((orientation * glm::vec3(0.0f, 1.0f, 0.0f)).z, 0.0f));

    Camera copy;
    copy.setPose(camera.position(),
                 orientation * glm::vec3(1.0f, 0.0f, 0.0f),
                 camera.distance);
    CHECK(testing::approx(copy.yaw, 2.1f));
    CHECK(testing::approx(copy.pitch, 0.7f));
  }

  return testing::report("cameraTests");
}
