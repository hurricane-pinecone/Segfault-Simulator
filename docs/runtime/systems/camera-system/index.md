# Camera system

`CameraSystem` moves a camera entity toward a target entity each frame using
exponential smoothing. Add it to your scene once and configure individual
cameras through `CameraComponent`.

## Setup

```cpp
CameraSystem& cameras = scene.addSystem<CameraSystem>();
```

## CameraComponent

Attach a `CameraComponent` to any entity that also has a `TransformComponent`.
The system moves that entity toward the configured target each update.

| Field | Type | Default | Purpose |
| --- | --- | --- | --- |
| `target` | `int` | `-1` | ID of the entity to follow. Resolves to that entity's `TransformComponent`. |
| `smoothing` | `float` | `8` | Lag coefficient. Higher values catch up faster; `0` means the camera does not move. |
| `offset` | `glm::vec2` | `(0, 0)` | World-space offset added to the target's position before smoothing is applied. |
| `zoom` | `float` | `1` | View scale factor. Values greater than `1` zoom in. |
| `shakeMaxOffset` | `float` | `0.5` | Peak displacement in world units at full shake strength. Tune per project. |

```cpp
Entity camera = scene.createEntity();
camera.addComponent<TransformComponent>(startPos, glm::vec2{1.0f, 1.0f}, 0.0f);
camera.addComponent<CameraComponent>(
    static_cast<int>(player.getId()),
    glm::vec2{0.0f, 2.0f}, // look slightly ahead of the target
    8.0f                    // smoothing: typical range 4 to 16
);
```

If `target` does not resolve to a live entity, the camera stays put.

## Smoothing

Each update the system applies one exponential step:

```
t        = 1 - exp(-smoothing * dt)
position = position + (desired - position) * t
```

`desired` is the target's position plus `offset`. At `smoothing = 8` and
`dt = 0.016` (60 fps), roughly 12 % of the remaining gap closes per frame.

## Screen shake

Call `shake()` on a `CameraComponent` to start a transient displacement that
fades out over the given duration. Calling it again while a shake is running
restarts it from the beginning.

```cpp
// Trigger a short shake on impact.
cam.shake(
    0.8f,  // strength: 0..1 (clamped), scales shakeMaxOffset
    0.3f,  // duration in seconds (default 0.5)
    1.5f   // decay exponent (default 1)
);
```

| Parameter | Type | Default | Purpose |
| --- | --- | --- | --- |
| `strength` | `float` | (required) | Peak amplitude as a fraction of `shakeMaxOffset`, clamped to 0..1. |
| `duration` | `float` | `0.5` | Length of the shake in seconds. |
| `decay` | `float` | `1` | Exponent for the fade envelope. `1` fades linearly; values above `1` drop quickly then tail off. |

The displacement is added to the camera's effective position and is visible in
`getCameraPosition()`. It does not modify the camera entity's
`TransformComponent`; the followed base position is unchanged.

`CameraSystem` advances the shake each frame automatically. You do not call
`updateShake()` yourself.

## Querying the active camera

`CameraSystem::activeCamera()` returns an `ActiveCamera` with pointers to the
first registered camera's `CameraComponent` and `TransformComponent`. Both
pointers are null before any camera entity is registered.

`ActiveCamera::getCameraPosition()` returns the world-space point the render
path should centre on: the smoothed followed position, plus the static offset,
plus the current shake displacement.

```cpp
const ActiveCamera active = cameras.activeCamera();
const glm::vec2 viewCentre = active.getCameraPosition();
```

Use `viewCentre` to feed the projection matrix each frame, or pass it to
`SpriteRenderSystem::setCameraOffset`.
