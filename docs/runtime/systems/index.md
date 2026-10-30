# Runtime systems

The runtime ships built-in systems for common mechanics. Add them to a scene
and they run each frame in the update phase.

- **[Camera system](./camera-system/index.md)**: makes camera entities follow
  something smoothly, and shake. The same in every style.
- **2D movement and collision** (below): `MovementSystem` and
  `CollisionSystem`, for bodies on a 2D game's plane.
- **[3D physics](../physics-3d/index.md)**: `PhysicsSystem3D`, rigid bodies
  and colliders on Jolt, for 3D games.

As in Unity, 2D and 3D physics are separate: the 2D components have plain
names (`RigidBody2DComponent`, `BoxCollider2D`), and 3D physics keeps its
suffix (`RigidBody3DComponent`, `BoxCollider3DComponent`, `PhysicsSystem3D`).

For how systems fit into the scene lifecycle, see
[Game & scenes](../game-and-scenes/index.md). For drawing, see
[Rendering](../rendering/index.md).

## 2D movement and collision

A 2D game's bodies move on its plane (`TransformComponent::position2D()`), in
the game's own units.

| Piece | What it does |
| --- | --- |
| `RigidBody2DComponent` | A body's `velocity` on the plane, in units per second |
| `MovementSystem` | Moves each `RigidBody2DComponent` entity by its velocity every frame. Given a terrain (`setTerrainHeightSource`, e.g. a `VoxelWorld`), it stops bodies at cliffs, stepping up one level at most. |
| `WorldCollider` | A body's footprint on the ground, in pixels from its feet, for blocking: solid objects carry it plus the `SolidObject` tag |
| `CollisionSystem` | Stops moving `WorldCollider` bodies against solid ones, by the side they came from |
| `BoxCollider2D` | An axis-aligned box on the plane (`half` extents, `offset`), for overlap tests: platforms, bullets, pickups, and what particles land on |

```cpp
addSystem<sfs::MovementSystem>();
addSystem<sfs::CollisionSystem>();

createEntity()
    .addComponent<sfs::TransformComponent>(glm::vec2{4.0f, 6.0f})
    .addComponent<sfs::RigidBody2DComponent>(glm::vec2{1.5f, 0.0f});
```

These are deliberately basic. A game with richer motion (a platformer's
jumping and landing, say) writes its own system over the same components, as
`samplePlatformer` does.
