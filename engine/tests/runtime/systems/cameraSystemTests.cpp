#include "../../testHarness.h"

#include <engine/core/components/boxCollider3DComponent.h>
#include <engine/core/components/cameraCollisionComponent.h>
#include <engine/core/components/cameraComponent.h>
#include <engine/core/components/cameraFollowComponent.h>
#include <engine/core/components/rigidBody3DComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/ecs/ecs.h>
#include <engine/core/hierarchy/hierarchy.h>
#include <engine/core/rendering/camera.h>
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
// A camera 20 m from its pivot at the origin, 0.3 rad above level, with
// camera collision, and blockers to put between them.
struct Orbit
{
  Registry reg;
  CameraSystem& sys = reg.addSystem<CameraSystem>();
  Camera view;
  Entity camera;

  Orbit()
  {
    view.target = glm::vec3(0.0f);
    view.yaw = 0.4f;
    view.pitch = 0.3f;
    view.distance = 20.0f;
    camera = reg.createEntity();
    camera.addComponent<TransformComponent>(
        TransformComponent(view.position(), view.orientation()));
    CameraComponent lens;
    lens.focusDistance = 20.0f;
    camera.addComponent<CameraComponent>(lens);
    camera.addComponent<CameraCollisionComponent>();
  }

  CameraCollisionComponent& collision()
  {
    return camera.getComponent<CameraCollisionComponent>();
  }

  // A static box standing `out` metres back from the pivot toward the eye,
  // its top at `top`, `tall` high.
  Entity box(float out, float top, float tall = 8.0f)
  {
    const glm::vec3 back = -view.groundForward() * out;
    Entity entity = reg.createEntity();
    entity.addComponent<TransformComponent>(
        TransformComponent(glm::vec3(back.x, back.y, top - tall * 0.5f)));
    BoxCollider3DComponent shape;
    shape.size = {4.0f, 4.0f, tall};
    entity.addComponent<BoxCollider3DComponent>(shape);
    reg.flushEntities();
    return entity;
  }

  // The camera's pitch now (radians above its pivot), and its distance.
  float pitch() const
  {
    const TransformComponent& t = camera.getComponent<TransformComponent>();
    Camera seen;
    seen.setPose(t.position, t.rotation * glm::vec3(1.0f, 0.0f, 0.0f), 20.0f);
    return seen.pitch;
  }
  float distance() const
  {
    return glm::length(camera.getComponent<TransformComponent>().position);
  }

  // The height of the eye's line to the pivot, `out` metres back.
  float lineAt(float out) const { return out * std::tan(pitch()); }
};
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
    CameraSystem::shake(camera, 1.0f, 1.0f, 1.0f); // full strength, 1s, linear
    sys.update(0.1);

    // Some displacement this frame, each axis bounded by strength * maxOffset.
    const glm::vec2 o = c.shakeOffset;
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
    CameraSystem::shake(camera, 1.0f, 0.5f);

    for (int i = 0; i < 10; i++) // 1.0s elapsed > 0.5s duration
      sys.update(0.1);

    CHECK(testing::approx(c.shakeOffset.x, 0.0f));
    CHECK(testing::approx(c.shakeOffset.y, 0.0f));
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
    CameraSystem::shake(camera, 5.0f, 1.0f); // over-strength: clamps to 1.0
    sys.update(0.05); // early in the shake, amplitude ~ strength

    CHECK(std::abs(c.shakeOffset.x) <= 1.0f + 1e-4f);
    CHECK(std::abs(c.shakeOffset.y) <= 1.0f + 1e-4f);
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

  TEST("a box between the eye and the pivot should lift the camera over it, "
       "keeping its distance")
  {
    Orbit orbit;
    orbit.reg.flushEntities();
    orbit.sys.lateUpdate(0.016);
    CHECK(testing::approx(orbit.pitch(), 0.3f, 1e-3f)); // nothing in the way
    orbit.box(10.0f, 8.0f);
    orbit.sys.lateUpdate(0.016);
    CHECK(orbit.pitch() > 0.4f);
    CHECK(orbit.lineAt(10.0f) >= 8.0f);
    CHECK(testing::approx(orbit.distance(), 20.0f, 1e-3f));
  }

  TEST("a blocker too short, on a layer masked out, moving, or a trigger "
       "shouldn't lift the camera")
  {
    {
      Orbit orbit;
      orbit.box(10.0f, 8.0f, 0.5f); // a slab under minHeight
      orbit.sys.lateUpdate(0.016);
      CHECK(testing::approx(orbit.pitch(), 0.3f, 1e-3f));
    }
    {
      Orbit orbit;
      orbit.box(10.0f, 8.0f).getComponent<BoxCollider3DComponent>().layer = 2;
      orbit.collision().layers = ~(1 << 2);
      orbit.sys.lateUpdate(0.016);
      CHECK(testing::approx(orbit.pitch(), 0.3f, 1e-3f));
    }
    {
      Orbit orbit;
      orbit.box(10.0f, 8.0f).getComponent<BoxCollider3DComponent>().trigger =
          true;
      orbit.sys.lateUpdate(0.016);
      CHECK(testing::approx(orbit.pitch(), 0.3f, 1e-3f));
    }
    {
      Orbit orbit;
      orbit.box(10.0f, 8.0f).addComponent<RigidBody3DComponent>();
      orbit.reg.flushEntities();
      orbit.sys.lateUpdate(0.016);
      CHECK(testing::approx(orbit.pitch(), 0.3f, 1e-3f));
      orbit.collision().movingBlocks = true;
      orbit.sys.lateUpdate(0.016);
      CHECK(orbit.pitch() > 0.4f);
    }
  }

  TEST("a camera left alone shouldn't stack lifts, and should ease back once "
       "clear")
  {
    Orbit orbit;
    Entity wall = orbit.box(10.0f, 8.0f);
    orbit.sys.lateUpdate(0.016);
    const float lifted = orbit.pitch();
    orbit.sys.lateUpdate(0.016);
    orbit.sys.lateUpdate(0.016);
    CHECK(testing::approx(orbit.pitch(), lifted, 1e-4f));

    wall.removeComponent<BoxCollider3DComponent>();
    orbit.sys.lateUpdate(0.1);
    const float easing = orbit.pitch();
    CHECK(easing < lifted && easing > 0.3f);
    // Eased by 1 - e^(-easeBack * dt) of the way.
    CHECK(testing::approx(
        easing - 0.3f, (lifted - 0.3f) * std::exp(-3.0f * 0.1f), 1e-3f));
    for (int i = 0; i < 100; ++i)
      orbit.sys.lateUpdate(0.1);
    CHECK(testing::approx(orbit.pitch(), 0.3f, 1e-3f));
  }

  TEST("a controller moving the camera should be lifted from its own pose")
  {
    Orbit orbit;
    orbit.box(10.0f, 8.0f);
    orbit.sys.lateUpdate(0.016);
    const float lifted = orbit.pitch();
    // The controller puts it back at its own pose each frame.
    for (int i = 0; i < 3; ++i)
    {
      orbit.camera.getComponent<TransformComponent>() =
          TransformComponent(orbit.view.position(), orbit.view.orientation());
      orbit.sys.lateUpdate(0.016);
      CHECK(testing::approx(orbit.pitch(), lifted, 1e-3f));
    }
  }

  TEST("a camera with its collision turned off shouldn't be lifted, its "
       "lift easing back as when clear")
  {
    Orbit orbit;
    orbit.box(10.0f, 8.0f);
    orbit.sys.lateUpdate(0.016);
    const float lift = orbit.collision().lift;
    CHECK(lift > 0.1f);

    orbit.collision().enabled = false;
    orbit.sys.lateUpdate(0.1);
    CHECK(testing::approx(orbit.pitch(), 0.3f, 1e-3f));
    CHECK(testing::approx(
        orbit.collision().lift, lift * std::exp(-3.0f * 0.1f), 1e-4f));
    // Moved by its controller while off: left where the controller put it.
    orbit.camera.getComponent<TransformComponent>() =
        TransformComponent(orbit.view.position(), orbit.view.orientation());
    orbit.sys.lateUpdate(0.1);
    CHECK(testing::approx(orbit.pitch(), 0.3f, 1e-3f));

    orbit.collision().enabled = true;
    orbit.sys.lateUpdate(0.016);
    CHECK(orbit.pitch() > 0.4f);
  }

  TEST("a lift should never pass maxPitch")
  {
    Orbit orbit;
    orbit.box(2.5f, 200.0f, 200.0f); // right behind the pivot
    orbit.sys.lateUpdate(0.016);
    CHECK(testing::approx(orbit.pitch(), glm::radians(80.0f), 1e-3f));
  }

  return testing::report("cameraSystemTests");
}
