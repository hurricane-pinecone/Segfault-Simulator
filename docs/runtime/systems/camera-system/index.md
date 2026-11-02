# Camera system

`CameraSystem` moves camera entities toward the entity they follow each frame,
with exponential smoothing, and runs their screen shake. It works the same in
every [style](../../rendering/styles/index.md): a flat or isometric camera
centres on the followed point, and a 3D one moves with it, keeping its own
orientation.

A camera entity is an entity with a `CameraComponent` (the lens: projection,
zoom, bloom...) and a `TransformComponent` (see
[Styles: the camera](../../rendering/styles/index.md#the-camera)). Following is
a separate component, `CameraFollowComponent`.

## Setup

```cpp
addSystem<sfs::CameraSystem>();

sfs::CameraComponent lens;
lens.orthographic = true;
lens.orthoHeight = 720.0f;

sfs::Entity camera = createEntity()
    .addComponent<sfs::TransformComponent>(startPosition)
    .addComponent<sfs::CameraComponent>(lens)
    .addComponent<sfs::CameraFollowComponent>(
        static_cast<int>(player.getId()),
        glm::vec3{0.0f, 2.0f, 0.0f}, // look slightly ahead of the target
        8.0f);                       // smoothing: typical range 4 to 16
```

Add the system after whatever moves the target, so the camera sees this
frame's position.

## CameraFollowComponent

| Field | Type | Default | Purpose |
| --- | --- | --- | --- |
| `target` | `int` | `-1` | Id of the entity to follow. `-1`, or an entity without a `TransformComponent`, leaves the camera where it is. |
| `offset` | `glm::vec3` | `(0, 0, 0)` | Added to the target's position |
| `smoothing` | `float` | `8` | How fast it catches up. Higher is snappier; `0` doesn't move. |
| `shakeMaxOffset` | `float` | `0.5` | Peak shake displacement at full strength, in world units. Tune per game. |

## Smoothing

Each update the system applies one exponential step:

```
t        = 1 - exp(-smoothing * dt)
position = position + (desired - position) * t
```

`desired` is the target's world position plus `offset`. At `smoothing = 8`
and `dt = 0.016` (60 fps), roughly 12 % of the remaining gap closes per frame.

Positions are world positions: a target that's a child of another entity is
followed where it is in the world, and a camera that's a child (on a rig, say)
is moved so its world position eases toward it.

## Screen shake

Call `shake()` on a `CameraFollowComponent` to start a displacement that
fades out over the given duration. Calling it again while a shake is running
restarts it.

```cpp
camera.getComponent<sfs::CameraFollowComponent>().shake(
    0.8f,  // strength: 0..1 (clamped), scales shakeMaxOffset
    0.3f,  // duration in seconds (default 0.5)
    1.5f); // decay exponent (default 1)
```

| Parameter | Type | Default | Purpose |
| --- | --- | --- | --- |
| `strength` | `float` | (required) | Peak amplitude as a fraction of `shakeMaxOffset`, clamped to 0..1 |
| `duration` | `float` | `0.5` | Length of the shake in seconds |
| `decay` | `float` | `1` | Exponent for the fade. `1` fades linearly; above `1` drops quickly then tails off. |

The shake moves the view across the screen; it doesn't change the camera
entity's `TransformComponent`. `CameraSystem` advances it each frame, so you
never call `updateShake()` yourself.

## Querying the active camera

`CameraSystem::activeCamera()` (or `findActiveCamera(registry)`) returns the
scene's first active camera entity (`CameraComponent` +
`TransformComponent`, in id order), or an invalid entity when there's none.
It's the camera the render system draws through.

```cpp
const sfs::Entity camera = getSystem<sfs::CameraSystem>().activeCamera();
if (camera.isValid())
{
  const glm::vec2 centre =
      sfs::hierarchy::worldTransform(camera).position2D(); // on the plane
  const float zoom = camera.getComponent<sfs::CameraComponent>().zoom;
}
```

Its transform already includes the follow's `offset` (the system moved it
there); the shake (`CameraFollowComponent::shakeOffset()`) is added only to
the view.
