#include "../../testHarness.h"

#include <engine/core/components/cameraComponent.h>
#include <engine/core/components/cameraFollowComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/ecs/ecs.h>
#include <engine/core/hierarchy/hierarchy.h>
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

Entity addCamera(Registry& reg,
                 glm::vec2 position,
                 const CameraFollowComponent& follow)
{
  Entity e = reg.createEntity();
  e.addComponent<TransformComponent>(TransformComponent{position});
  e.addComponent<CameraComponent>();
  e.addComponent<CameraFollowComponent>(follow);
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

    CameraFollowComponent cam;
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

    CameraFollowComponent cam;
    cam.target = static_cast<int>(target.getId());
    cam.smoothing = 8.0f;
    cam.offset = {0.0f, 4.0f, 0.0f}; // desired = target + offset
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

    CameraFollowComponent cam;
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
    CHECK(!sys.activeCamera().isValid()); // none registered yet

    CameraFollowComponent cam;
    Entity camera = addCamera(reg, {1.0f, 2.0f}, cam);
    reg.flushEntities();

    CHECK(sys.activeCamera() == camera);
  }

  TEST("activeCamera should skip inactive cameras, and need no follow")
  {
    Registry reg;
    CameraSystem& sys = reg.addSystem<CameraSystem>();

    Entity off = reg.createEntity();
    off.addComponent<TransformComponent>(
        TransformComponent{glm::vec2{1.0f, 0.0f}});
    off.addComponent<CameraComponent>();
    off.getComponent<CameraComponent>().active = false;

    Entity on = reg.createEntity();
    on.addComponent<TransformComponent>(
        TransformComponent{glm::vec2{4.0f, 0.0f}});
    on.addComponent<CameraComponent>();
    on.getComponent<CameraComponent>().zoom = 2.0f;
    reg.flushEntities();

    CHECK(sys.activeCamera() == on);
  }

  TEST("a 3D camera should follow its target in all three axes")
  {
    Registry reg;
    CameraSystem& sys = reg.addSystem<CameraSystem>();

    Entity target = reg.createEntity();
    target.addComponent<TransformComponent>(
        TransformComponent{glm::vec3{0.0f, 0.0f, 10.0f}});

    CameraFollowComponent cam;
    cam.target = static_cast<int>(target.getId());
    cam.offset = {0.0f, -5.0f, 2.0f};
    Entity camera = addCamera(reg, {0.0f, 0.0f}, cam);
    reg.flushEntities();

    sys.update(0.1);

    const auto& t = camera.getComponent<TransformComponent>();
    CHECK(testing::approx(t.position.y, smoothed(0.0f, -5.0f, 8.0f, 0.1f)));
    CHECK(testing::approx(t.position.z, smoothed(0.0f, 12.0f, 8.0f, 0.1f)));
  }

  TEST("shake produces a bounded offset without moving the base position")
  {
    Registry reg;
    CameraSystem& sys = reg.addSystem<CameraSystem>();

    // No valid target, so the followed base position stays put and only the
    // shake offset can change.
    CameraFollowComponent cam;
    cam.target = 9999;
    cam.shakeMaxOffset = 1.0f;
    Entity camera = addCamera(reg, {5.0f, 5.0f}, cam);
    reg.flushEntities();

    auto& c = camera.getComponent<CameraFollowComponent>();
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

    CameraFollowComponent cam;
    cam.target = 9999;
    cam.shakeMaxOffset = 1.0f;
    Entity camera = addCamera(reg, {0.0f, 0.0f}, cam);
    reg.flushEntities();

    auto& c = camera.getComponent<CameraFollowComponent>();
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

    CameraFollowComponent cam;
    cam.target = 9999;
    cam.shakeMaxOffset = 1.0f;
    Entity camera = addCamera(reg, {0.0f, 0.0f}, cam);
    reg.flushEntities();

    auto& c = camera.getComponent<CameraFollowComponent>();
    c.shake(5.0f, 1.0f); // over-strength: clamps to 1.0
    sys.update(0.05);    // early in the shake, amplitude ~ strength

    CHECK(std::abs(c.shakeOffset().x) <= 1.0f + 1e-4f);
    CHECK(std::abs(c.shakeOffset().y) <= 1.0f + 1e-4f);
  }

  TEST("a parented camera should follow its target in the world")
  {
    Registry reg;
    CameraSystem& sys = reg.addSystem<CameraSystem>();

    Entity target = reg.createEntity();
    target.addComponent<TransformComponent>(
        TransformComponent{glm::vec2{10.0f, 4.0f}});

    // The camera hangs off a rig moved 6 along x: its own position is
    // relative to that.
    Entity rig = reg.createEntity();
    rig.addComponent<TransformComponent>(
        TransformComponent{glm::vec2{6.0f, 0.0f}});
    CameraFollowComponent cam;
    cam.target = target.getId();
    cam.offset = glm::vec3(0.0f, 1.0f, 0.0f);
    cam.smoothing = 1000.0f; // snaps
    Entity camera = addCamera(reg, {0.0f, 0.0f}, cam);
    hierarchy::setParent(camera, rig);
    reg.flushEntities();

    sys.update(0.1);

    const glm::vec3 world = hierarchy::worldPosition(camera);
    CHECK(testing::approx(world.x, 10.0f));
    CHECK(testing::approx(world.y, 5.0f));
    CHECK(testing::approx(
        camera.getComponent<TransformComponent>().position.x, 4.0f));
  }

  return testing::report("cameraSystemTests");
}
