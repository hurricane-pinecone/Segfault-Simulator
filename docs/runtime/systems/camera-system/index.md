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
        player,
        glm::vec3{0.0f, 2.0f, 0.0f}, // look slightly ahead of the target
        8.0f);                       // smoothing: typical range 4 to 16
```

Add the system after whatever moves the target, so the camera sees this
frame's position.

## CameraFollowComponent

| Field | Type | Default | Purpose |
| --- | --- | --- | --- |
| `target` | `sfs::Entity` | none | The entity to follow, saved as a [reference](../../serialization/index.md#entity-references). None, a destroyed entity or one without a `TransformComponent` leaves the camera where it is. |
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

Call `CameraSystem::shake()` on a camera entity with a
`CameraFollowComponent` to start a displacement that fades out over the given
duration. Calling it again while a shake is running restarts it. The shake
under way is kept in the component's hidden fields (`shakeOffset`,
`shakeStrength`...); the system advances it.

```cpp
sfs::CameraSystem::shake(
    camera,
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

## Camera collision

A **`CameraCollisionComponent`** on a camera entity keeps its view of what it
looks at clear of solid things, as Unreal's SpringArm and Unity Cinemachine's
Deoccluder do. Its pivot is the point `focusDistance` (CameraComponent) ahead
of the eye. Where something stands between the eye and the pivot, the camera
pitches up about the pivot until its line to the pivot passes over it,
keeping its distance (Cinemachine's "preserve camera distance"). It goes up at
once and eases back down once clear.

`CameraSystem` does it in `lateUpdate`, after every game object's update, so
the camera's controller (an orbit script, a follow) places it as it likes and
the lift goes on top. A camera nothing moved since it was lifted is lifted
from its controller's pose again, so lifts never stack.

What blocks: static colliders (`BoxCollider3DComponent`,
`SphereCollider3DComponent`, `CapsuleCollider3DComponent` on entities without
a `RigidBody3DComponent`) within reach of the view, by their own shapes. It
tests the line at `samples` points along the ground from the pivot back to
under the eye. Each point wants the line to pass over the top of what stands
there (found by a ray straight down onto each shape), by a clearance that
grows from `targetClearance` at the pivot to `eyeClearance` at the eye. Points
within a metre of the pivot are skipped. The steepest pitch any point asks
for is the camera's, worked out twice (raising the pitch brings the eye in
over the ground), and never past `maxPitch`.

| Field | Meaning |
| --- | --- |
| `layers` | The collider layers that block, as a bit mask (bit n: layer n; -1: all). Leave out the layer of what should be seen through, such as trees that fade. |
| `minHeight` | Blockers less tall than this are seen over (fences, kerbs) |
| `targetClearance`, `eyeClearance` | How far the line keeps above blockers at the pivot end and at the eye, in metres |
| `samples` | How many points along the line are tested |
| `maxPitch` | The steepest it ever looks down, in degrees below level |
| `easeBack` | How quickly the lift eases back down once clear, per second |
| `movingBlocks` | Bodies with a `RigidBody3DComponent` block too |
| `enabled` | Whether it lifts the camera now. Off, the controller's pose is left as it is and the lift eases back as when nothing blocks: turn it off for a view that needs none, such as a driver's seat |

```cpp
sfs::CameraCollisionComponent collision;
collision.layers = ~(1 << 2); // trees (layer 2) fade instead
camera.addComponent<sfs::CameraCollisionComponent>(collision);
```

Terrain without a collider doesn't block: a controller that knows its ground
keeps its own clearance over it, and the lift goes on top.

## Querying the active camera

`CameraSystem::activeCamera()` (or `findActiveCamera(registry)`) returns the
scene's first active camera entity (`CameraComponent` +
`TransformComponent`, in id order), or an invalid entity when there's none.
It's the camera the render system draws through.

```cpp
const sfs::Entity camera = getSystem<sfs::CameraSystem>().activeCamera();
if (const auto* lens = camera.getComponent<sfs::CameraComponent>())
{
  const glm::vec2 centre =
      sfs::hierarchy::worldTransform(camera).position2D(); // on the plane
  const float zoom = lens->zoom;
}
```

Its transform already includes the follow's `offset` (the system moved it
there); the shake (`CameraFollowComponent::shakeOffset()`) is added only to
the view.
