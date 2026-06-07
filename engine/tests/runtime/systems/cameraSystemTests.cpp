#include "../../testHarness.h"

#include <engine/core/components/cameraComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/ecs/ecs.h>
#include <engine/runtime/systems/cameraSystem.h>

#include <cmath>

using namespace sfs;

namespace
{
// The smoothing lerp the system applies in one step toward a desired point.
float smoothed(float from, float to, float smoothing, float dt)
{
  const float t = 1.0f - std::exp(-smoothing * dt);
  return from + (to - from) * t;
}

Entity addCamera(Registry& reg, glm::vec2 position, const CameraComponent& cam)
{
  Entity e = reg.createEntity();
  e.addComponent<TransformComponent>(TransformComponent{position});
  e.addComponent<CameraComponent>(cam);
  return e;
}
} // namespace

int main()
{
  TEST("a camera should ease toward its target")
  {
    Registry reg;
    CameraSystem& sys = reg.addSystem<CameraSystem>();

    Entity target = reg.createEntity();
    target.addComponent<TransformComponent>(TransformComponent{{10.0f, 0.0f}});

    CameraComponent cam;
    cam.target = static_cast<int>(target.getId());
    cam.smoothing = 8.0f;
    Entity camera = addCamera(reg, {0.0f, 0.0f}, cam);
    reg.flushEntities();

    sys.update(0.1);

    const auto& t = camera.getComponent<TransformComponent>();
    CHECK(testing::approx(t.position.x, smoothed(0.0f, 10.0f, 8.0f, 0.1f)));
    CHECK(testing::approx(t.position.y, 0.0f));
  }

  TEST("the camera offset should shift the followed point")
  {
    Registry reg;
    CameraSystem& sys = reg.addSystem<CameraSystem>();

    Entity target = reg.createEntity();
    target.addComponent<TransformComponent>(TransformComponent{{10.0f, 0.0f}});

    CameraComponent cam;
    cam.target = static_cast<int>(target.getId());
    cam.smoothing = 8.0f;
    cam.offset = {0.0f, 4.0f}; // desired = target + offset
    Entity camera = addCamera(reg, {0.0f, 0.0f}, cam);
    reg.flushEntities();

    sys.update(0.1);

    const auto& t = camera.getComponent<TransformComponent>();
    CHECK(testing::approx(t.position.y, smoothed(0.0f, 4.0f, 8.0f, 0.1f)));
  }

  TEST("a camera with no valid target should not move")
  {
    Registry reg;
    CameraSystem& sys = reg.addSystem<CameraSystem>();

    CameraComponent cam;
    cam.target = 9999; // no such entity
    Entity camera = addCamera(reg, {3.0f, 7.0f}, cam);
    reg.flushEntities();

    sys.update(0.1);

    const auto& t = camera.getComponent<TransformComponent>();
    CHECK(testing::approx(t.position.x, 3.0f));
    CHECK(testing::approx(t.position.y, 7.0f));
  }

  TEST("activeCamera should expose the first camera, or nothing")
  {
    Registry reg;
    CameraSystem& sys = reg.addSystem<CameraSystem>();
    CHECK(sys.activeCamera().camera == nullptr); // none registered yet

    CameraComponent cam;
    Entity camera = addCamera(reg, {1.0f, 2.0f}, cam);
    reg.flushEntities();

    const ActiveCamera active = sys.activeCamera();
    CHECK(active.camera != nullptr);
    CHECK(active.transform != nullptr);
    CHECK(testing::approx(active.transform->position.x, 1.0f));
  }

  return testing::report("cameraSystemTests");
}
