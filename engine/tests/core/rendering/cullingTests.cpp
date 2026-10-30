#include "../../testHarness.h"

#include <engine/core/rendering/aabb.h>
#include <engine/core/rendering/camera.h>
#include <engine/core/rendering/frustum.h>
#include <engine/core/rendering/sunShadow.h>

#include <glm/glm/ext/matrix_transform.hpp>
#include <glm/glm/ext/scalar_constants.hpp>

using namespace sfs;

namespace
{
Aabb boxAt(const glm::vec3& center, float halfSize)
{
  Aabb box;
  box.expand(center - glm::vec3(halfSize));
  box.expand(center + glm::vec3(halfSize));
  return box;
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
  TEST("a default box should be empty and grow to fit points")
  {
    Aabb box;
    CHECK(box.empty());
    box.expand(glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK(!box.empty());
    box.expand(glm::vec3(-1.0f, 0.0f, 5.0f));
    CHECK(testing::approx(box.min.x, -1.0f));
    CHECK(testing::approx(box.max.z, 5.0f));
  }

  TEST("transforming a box should enclose its rotated corners")
  {
    const Aabb unit = boxAt({0.0f, 0.0f, 0.0f}, 1.0f);
    const glm::mat4 rotate = glm::rotate(
        glm::mat4(1.0f), glm::pi<float>() / 4.0f, glm::vec3(0.0f, 0.0f, 1.0f));
    const Aabb rotated = unit.transformed(rotate);
    // A unit square turned 45 degrees reaches sqrt(2) along X and Y.
    CHECK(testing::approx(rotated.max.x, 1.41421f, 1e-3f));
    CHECK(testing::approx(rotated.max.z, 1.0f, 1e-4f));
  }

  TEST("transforming a box should translate and scale it")
  {
    const glm::mat4 m =
        glm::scale(glm::translate(glm::mat4(1.0f), {10.0f, 0.0f, 0.0f}),
                   {2.0f, 2.0f, 2.0f});
    const Aabb moved = boxAt({0.0f, 0.0f, 0.0f}, 1.0f).transformed(m);
    CHECK(testing::approx(moved.min.x, 8.0f));
    CHECK(testing::approx(moved.max.x, 12.0f));
  }

  TEST("the frustum should keep boxes in view and drop ones behind")
  {
    Camera camera;
    camera.target = {0.0f, 0.0f, 0.0f};
    camera.yaw = 0.0f; // looking along +X
    camera.distance = 20.0f;
    const Frustum frustum =
        Frustum::fromViewProjection(camera.viewProjection(1.0f));

    CHECK(frustum.intersects(boxAt(camera.target, 1.0f)));
    CHECK(!frustum.intersects(
        boxAt(camera.position() - glm::vec3(30, 0, 0), 1.0f)));
    CHECK(!frustum.intersects(boxAt({0.0f, 400.0f, 0.0f}, 1.0f)));
    CHECK(!frustum.intersects(Aabb{}));
  }

  TEST("a box straddling the frustum edge should be kept")
  {
    Camera camera;
    camera.projection = Camera::Projection::Orthographic;
    camera.orthoHeight = 10.0f;
    const Frustum frustum =
        Frustum::fromViewProjection(camera.viewProjection(1.0f));
    // Ortho half-width is 5 along the camera's right axis.
    const glm::vec3 edge = camera.target + camera.groundRight() * 5.0f;
    CHECK(frustum.intersects(boxAt(edge, 0.5f)));
    CHECK(!frustum.intersects(
        boxAt(camera.target + camera.groundRight() * 7.0f, 0.5f)));
  }

  TEST("the sun shadow volume should contain the covered region")
  {
    const glm::vec3 center{12.0f, -4.0f, 0.0f};
    const glm::mat4 light =
        fitSunShadow({-0.4f, -0.3f, 0.85f}, center, 20.0f, 2048);

    // Points just inside the covered sphere of radius 20.
    for (const glm::vec3 offset : {glm::vec3(13, 13, 0),
                                   glm::vec3(-13, 13, 4),
                                   glm::vec3(13, -13, -4),
                                   glm::vec3(0, 0, 19),
                                   glm::vec3(0, 0, 0)})
    {
      const glm::vec3 ndc = project(light, center + offset);
      CHECK(ndc.x > -1.0f && ndc.x < 1.0f);
      CHECK(ndc.y > -1.0f && ndc.y < 1.0f);
      CHECK(ndc.z > 0.0f && ndc.z < 1.0f);
    }
  }

  TEST("the sun shadow should not jitter by less than a texel")
  {
    // Moving the covered region by a tiny step must move the projection of a
    // fixed world point by a whole number of texels (or not at all).
    const glm::vec3 sun{-0.4f, -0.3f, 0.85f};
    const uint32_t resolution = 1024;
    const glm::mat4 a =
        fitSunShadow(sun, {0.0f, 0.0f, 0.0f}, 20.0f, resolution);
    const glm::mat4 b =
        fitSunShadow(sun, {0.013f, 0.0f, 0.0f}, 20.0f, resolution);

    const glm::vec3 point{3.0f, 2.0f, 1.0f};
    const float texels = (project(a, point).x - project(b, point).x) *
                         static_cast<float>(resolution) * 0.5f;
    CHECK(testing::approx(texels, std::round(texels), 1e-2f));
  }

  TEST("an overhead sun should still give a valid shadow volume")
  {
    const glm::mat4 light =
        fitSunShadow({0.0f, 0.0f, 1.0f}, {0, 0, 0}, 10.0f, 512);
    const glm::vec3 ndc = project(light, {0.0f, 0.0f, 0.0f});
    CHECK(testing::approx(ndc.x, 0.0f, 1e-2f));
    CHECK(ndc.z > 0.0f && ndc.z < 1.0f);
  }

  return testing::report("cullingTests");
}
