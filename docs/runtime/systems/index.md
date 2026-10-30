# Runtime systems

The runtime ships built-in systems for common mechanics. Add them to a scene
(in code, or by name in the scene's systems list) and they run each frame in
the update phase.

- **[Camera system](./camera-system/index.md)**: makes camera entities follow
  something smoothly, and shake. The same in every style.
- **2D physics** (below): `PhysicsSystem2D`, bodies and boxes on a 2D game's
  plane, with contacts told to game objects.
- **[3D physics](../physics-3d/index.md)**: `PhysicsSystem3D`, rigid bodies
  and colliders on Jolt, for 3D games.

As in Unity, 2D and 3D physics are separate: the 2D components have plain
names (`RigidBody2DComponent`, `BoxCollider2D`), and 3D physics keeps its
suffix (`RigidBody3DComponent`, `BoxCollider3DComponent`, `PhysicsSystem3D`).

For how systems fit into the scene lifecycle, see
[Game & scenes](../game-and-scenes/index.md). For drawing, see
[Rendering](../rendering/index.md).

## 2D physics

A 2D game's bodies move on its plane (`TransformComponent::position2D()`), in
the game's own units: a flat game's pixels, an isometric game's tiles.

| Piece | What it does |
| --- | --- |
| `RigidBody2DComponent` | A body: its `velocity` and its `gravity` (how fast it speeds up falling; zero for a top-down game, pointing down for a platformer), in units per second |
| `BoxCollider2D` | An axis-aligned box on the plane: `half` extents, an `offset` from the entity, and a `trigger` flag. On an entity without a body it's a static: a wall, a floor, a platform |
| `PhysicsSystem2D` | Moves the bodies and stops them against static boxes, and reports contacts |
| `SolidObject` | A tag for statics that particles bounce off and stain (see [Particles](../particles/index.md)); the physics doesn't need it |

Each update `PhysicsSystem2D`:

1. speeds each body up by its gravity, and moves it by its velocity;
2. if the body has a (non-trigger) box, sweeps it across, then up or down,
   stopping flush against the first static box in the way. A fast body can't
   pass through a thin wall, it slides along what it hits, and its velocity
   into it is zeroed;
3. finds what touches what, and reports the contacts that began and ended.

Bodies don't push each other (they pass through), but they're told when they
touch. A trigger blocks nothing; it's told what overlaps it. Things only meet
at the same height (the transform's z, rounded), so an isometric game's levels
don't collide with each other. Positions are world positions: a body may be a
child of another entity, and it's moved in the world.

```cpp
addSystem<sfs::PhysicsSystem2D>(this); // `this`: the scene, told of contacts

// A floor.
sfs::Entity floor = createEntity();
floor.addComponent<sfs::TransformComponent>(glm::vec2{0.0f, -16.0f});
floor.addComponent<sfs::BoxCollider2D>(glm::vec2{400.0f, 16.0f});

// A body that falls onto it.
sfs::Entity crate = createEntity();
crate.addComponent<sfs::TransformComponent>(glm::vec2{0.0f, 200.0f});
crate.addComponent<sfs::BoxCollider2D>(glm::vec2{8.0f, 8.0f});
crate.addComponent<sfs::RigidBody2DComponent>(glm::vec2{0.0f},
                                              glm::vec2{0.0f, -900.0f});
```

A scene file asks for it by listing `"PhysicsSystem2D"` among its systems (the
editor's Scene Settings > Systems), which also hands it the scene.

### Contacts

A game object on either entity hears about a contact (a `Collision2D`, with
the `other` entity and the `normal` it was pushed along, zero for a trigger):

```cpp
class Crate : public sfs::GameObject
{
protected:
  void onCollisionEnter2D(const sfs::Collision2D& hit) override
  {
    if (hit.normal.y > 0.5f) // pushed up: landed on something
      landed = true;
  }
  void onTriggerEnter2D(const sfs::Collision2D& zone) override { /* ... */ }

  bool landed = false;
};
```

`onCollisionExit2D` and `onTriggerExit2D` say when it ends. A body resting on
a floor stays one contact. The system tells game objects as it updates (before
the objects' own `onUpdate`); `contactsBegun()` and `contactsEnded()` list the
last update's contacts (`Contact2D`) too.

The physics is deliberately simple: boxes, no rotation, no pushing between
bodies. A game with richer motion writes its own system over the same
components.
