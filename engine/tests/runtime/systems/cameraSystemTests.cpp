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

  TEST("shake produces a bounded offset without moving the base position")
  {
    Registry reg;
    CameraSystem& sys = reg.addSystem<CameraSystem>();

    // No valid target, so the followed base position stays put and only the
    // shake offset can change.
    CameraComponent cam;
    cam.target = 9999;
    cam.shakeMaxOffset = 1.0f;
    Entity camera = addCamera(reg, {5.0f, 5.0f}, cam);
    reg.flushEntities();

    auto& c = camera.getComponent<CameraComponent>();
    c.shake(1.0f, 1.0f, 1.0f); // full strength, 1s, linear
    sys.update(0.1);

    // Some displacement this frame, each axis bounded by strength * maxOffset.
    const glm::vec2 o = c.shakeOffset();
    CHECK(std::abs(o.x) > 0.0f || std::abs(o.y) > 0.0f);
    CHECK(std::abs(o.x) <= 1.0f + 1e-4f);
    CHECK(std::abs(o.y) <= 1.0f + 1e-4f);

    // The base transform is untouched by the shake (no drift).
    const auto& t = camera.getComponent<TransformComponent>();
    CHECK(testing::approx(t.position.x, 5.0f));
    CHECK(testing::approx(t.position.y, 5.0f));
  }

  TEST("shake fades to nothing after its duration")
  {
    Registry reg;
    CameraSystem& sys = reg.addSystem<CameraSystem>();

    CameraComponent cam;
    cam.target = 9999;
    cam.shakeMaxOffset = 1.0f;
    Entity camera = addCamera(reg, {0.0f, 0.0f}, cam);
    reg.flushEntities();

    auto& c = camera.getComponent<CameraComponent>();
    c.shake(1.0f, 0.5f);

    for (int i = 0; i < 10; i++) // 1.0s elapsed > 0.5s duration
      sys.update(0.1);

    CHECK(testing::approx(c.shakeOffset().x, 0.0f));
    CHECK(testing::approx(c.shakeOffset().y, 0.0f));
  }

  TEST("shake clamps strength so the offset never exceeds maxOffset")
  {
    Registry reg;
    CameraSystem& sys = reg.addSystem<CameraSystem>();

    CameraComponent cam;
    cam.target = 9999;
    cam.shakeMaxOffset = 1.0f;
    Entity camera = addCamera(reg, {0.0f, 0.0f}, cam);
    reg.flushEntities();

    auto& c = camera.getComponent<CameraComponent>();
    c.shake(5.0f, 1.0f); // over-strength: clamps to 1.0
    sys.update(0.05);    // early in the shake, amplitude ~ strength

    CHECK(std::abs(c.shakeOffset().x) <= 1.0f + 1e-4f);
    CHECK(std::abs(c.shakeOffset().y) <= 1.0f + 1e-4f);
  }

  TEST("the active camera position includes the shake offset")
  {
    Registry reg;
    CameraSystem& sys = reg.addSystem<CameraSystem>();

    CameraComponent cam;
    cam.target = 9999;
    cam.shakeMaxOffset = 1.0f;
    Entity camera = addCamera(reg, {2.0f, 3.0f}, cam);
    reg.flushEntities();

    auto& c = camera.getComponent<CameraComponent>();
    c.shake(1.0f, 1.0f);
    sys.update(0.1);

    const glm::vec2 pos = sys.activeCamera().getCameraPosition();
    CHECK(testing::approx(pos.x, 2.0f + c.shakeOffset().x));
    CHECK(testing::approx(pos.y, 3.0f + c.shakeOffset().y));
  }

  return testing::report("cameraSystemTests");
}
