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

| Field | Type | Purpose |
| --- | --- | --- |
| `target` | `int` | ID of the entity to follow. Resolves to that entity's `TransformComponent`. |
| `smoothing` | `float` | Lag coefficient. Higher values catch up faster; `0` means the camera does not move. |
| `offset` | `glm::vec2` | World-space offset added to the target's position before the system applies smoothing. |

```cpp
Entity camera = scene.createEntity();
camera.addComponent<TransformComponent>(TransformComponent{{0.0f, 0.0f}});

CameraComponent cam;
cam.target    = static_cast<int>(player.getId());
cam.smoothing = 8.0f;        // typical range: 4 to 16
cam.offset    = {0.0f, 2.0f}; // look slightly ahead of the target
camera.addComponent<CameraComponent>(cam);
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

## Querying the active camera

`CameraSystem::activeCamera()` returns an `ActiveCamera` with pointers to the
first registered camera's `CameraComponent` and `TransformComponent`. Both
pointers are null before any camera entity is registered.

```cpp
const ActiveCamera active = cameras.activeCamera();
if (active.camera && active.transform) {
    glm::vec2 viewCentre = active.transform->position;
}
```

Use `viewCentre` to feed the projection matrix each frame, or pass it to
`SpriteRenderSystem::setCameraOffset`.
