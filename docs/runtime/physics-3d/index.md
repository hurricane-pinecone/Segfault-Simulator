# 3D physics

3D games get rigid-body physics from `PhysicsSystem3D`, built on
[Jolt Physics](https://github.com/jrouwe/JoltPhysics). As in Unity, it keeps
its 3D suffix (`RigidBody3DComponent`, `BoxCollider3DComponent`...) to tell it
from a 2D game's physics (`PhysicsSystem2D`, `RigidBody2DComponent`,
`BoxCollider2D`; see [Runtime systems](../systems/index.md#2d-physics)).
The web build has no 3D physics.

## Turning it on

Physics is optional, per scene, so a scene that doesn't need it pays nothing for
it. A scene that wants it lists `PhysicsSystem3D` among its systems, the same
list its game systems go in:

- **In the editor:** Scene Settings → Systems → **Add System** → *Engine:*
  PhysicsSystem3D.
- **In the scene file:** `"systems": ["PhysicsSystem3D", ...]`.

Like a game system, it's created when the scene plays, and only then: in the
editor it doesn't exist while you edit. Code that may run in a scene without
physics should ask with `StandardScene::physicsSystem()` or
`scene().tryGetSystem<sfs::PhysicsSystem3D>()`, which return null when it's
off. `getSystem` stops the game with an error when the system is missing.

The collider and rigid body components are plain data, so a game can also leave
`PhysicsSystem3D` out and write its own System that reads them.

## Bodies from components

An entity with a `TransformComponent` and at least one collider is a physics
body:

| Components | Body |
| --- | --- |
| A collider | **Static**: other bodies hit it, but it never moves (ground, walls, buildings) |
| A collider + `RigidBody3DComponent` | **Dynamic**: falls, collides and tumbles. Physics writes its position and rotation into its transform. |
| A collider + `RigidBody3DComponent` with `kinematic` on | **Kinematic**: your code moves its transform, and it pushes dynamic bodies without being pushed back (doors, lifts, a character driven by script) |

Colliders:

| Component | Shape |
| --- | --- |
| `BoxCollider3DComponent` | A box, `size` along the entity's axes |
| `SphereCollider3DComponent` | A sphere of `radius` |
| `CapsuleCollider3DComponent` | An upright capsule (`radius`, total `height` along Z), the usual shape for characters |

Each has an `offset` from the entity's origin, and a `trigger` flag (see below).
Colliders turn and scale with the entity. A sphere scales by the entity's
largest scale axis, so it stays round. An entity may have one of each kind; they
combine into one shape.

In the editor, a collider added to an entity with a mesh starts out fitted to
the bounds of its mesh and its children's (so one on a group covers the group),
and **Fit to Mesh** on the collider refits it (after the meshes or their sizes
change). A flat mesh, such as the built-in plane, gets a 0.1 m thick slab under
its surface.

`RigidBody3DComponent` fields: `mass` (kg), `kinematic`, `friction`,
`bounciness`, `linearDamping`, `angularDamping`, `gravityScale` (0 floats),
`lockRotation` (never tips over, e.g. a character) and `bendsGrass` (flattens
[grass](../../editor/index.md#grass) it moves through, sized by its colliders;
on by default).

The world is Z-up, in metres. Gravity defaults to (0, 0, −9.81);
`physics.setGravity` changes it.

## Moving bodies from code

- **Dynamic bodies** move by physics. Push them with
  `physics.addImpulse(entity, impulse)` (a sudden push, e.g. a jump),
  `addForce` (a steady push; call it every frame) or `setVelocity`.
  Setting the transform teleports the body.
- **Kinematic and static bodies** move by their transform. A kinematic body
  moved this way sweeps through the world, pushing what's in its way.

A dynamic body can be a child. Physics works in world space and writes the
result back relative to the parent.

## Collisions and triggers

A game object hears about its entity's contacts:

```cpp
class Pickup : public sfs::GameObject
{
protected:
  void onTriggerEnter(const sfs::Collision3D& collision) override
  {
    if (collision.other == player)
      scene().destroyBranch(entity());
  }
};
```

| Callback | When |
| --- | --- |
| `onCollisionEnter(collision)` / `onCollisionExit` | It starts or stops touching a solid body |
| `onTriggerEnter(collision)` / `onTriggerExit` | It starts or stops overlapping, where one of the two is a trigger |

`collision.other` is the entity it touched. Each pair reports one enter and one
exit, however many points touch. `PhysicsSystem3D` tells the game objects
itself, as it updates (before the objects' own `onUpdate`s), when it knows its
scene: a scene that lists it gets one that does. One made in code is told with
`addSystem<sfs::PhysicsSystem3D>(&scene)`.

A **trigger** collider detects bodies passing through instead of blocking them,
like a pickup, a checkpoint or a zone that opens a door. An entity whose
colliders are all triggers is a trigger. A static trigger notices moving
bodies; a kinematic one notices static bodies too.

`physics.contactsBegun()` and `contactsEnded()` list the same contacts for code
that isn't a game object, such as a System script.

## Raycasts

```cpp
auto& render = scene().getSystem<sfs::RenderSystem>();
auto& physics = scene().getSystem<sfs::PhysicsSystem3D>();
const sfs::Ray ray = render.screenRay(input.mouse().getPosition());
if (const auto hit = physics.raycast(ray, 200.0f))
  placeBuilding(hit->point, hit->normal);
```

`raycast` returns the nearest solid body the ray hits: its `entity`, the
`point`, the surface `normal` and the `distance`. Triggers are skipped. Raycasts
see the bodies as of the last physics update, so only while the scene plays.

## How it runs

Each frame, before the game objects update, the system:

1. Picks up bodies that were added, removed, or changed (a collider or rigid
   body field, or the entity's scale, which rebuilds the body) and transforms
   moved by code.
2. Steps the simulation at a fixed 60 steps a second, at most 4 per frame.
   After a long hitch it drops the rest rather than trying to catch up.
3. Writes dynamic bodies' new positions into their transforms.

The engine's prebuilt Jolt package targets AVX2 CPUs, so games need an x86-64
CPU from about 2013 or later. Physics isn't part of the web build.

## In the editor

- Physics is on only in scenes that list it (see
  [Turning it on](#turning-it-on)).
- Nothing simulates while you edit. Press **Play** to see bodies fall, and
  **Stop** to put everything back.
- Add colliders and rigid bodies with **Add Component**.
- The selection's colliders are drawn in the viewport, green for solid and
  orange for triggers (with **Gizmos** on).
