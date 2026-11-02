# Parents & children

Any entity can be the child of another. A child's `TransformComponent` is
relative to its parent: move, turn or scale the parent and its children follow.
A cart carries its crates, a lantern hangs from its post, and a "Fences" entity
can hold every fence segment so the editor's Hierarchy stays tidy.

```cpp
#include "engine/core/hierarchy/hierarchy.h"

sfs::Entity cart = scene.createEntity();
cart.addComponent<sfs::TransformComponent>(sfs::TransformComponent{{10.0f, 0.0f, 0.0f}});

sfs::Entity crate = scene.createEntity();
crate.addComponent<sfs::TransformComponent>(sfs::TransformComponent{{0.0f, 1.0f, 0.5f}});
sfs::hierarchy::setParent(crate, cart);

sfs::hierarchy::worldPosition(crate); // (10, 1, 0.5)
```

The link is a `ParentComponent` on the child. Change it through
`hierarchy::setParent`, which refuses to make a loop (an entity under its own
child), rather than writing the component directly.

## Functions

All are in `sfs::hierarchy`.

| Function | Does |
| --- | --- |
| `setParent(child, parent)` | Puts `child` under `parent`, keeping its local transform, so it moves to sit relative to the parent. An invalid `parent` makes it a root. Returns false, changing nothing, if it would make a loop. |
| `setParentKeepWorld(child, parent)` | The same, but the child stays where it is in the world: its local transform is recalculated. This is what the editor does when you drag in the Hierarchy. |
| `parentOf(entity)` | The parent, or an invalid entity for a root |
| `isAncestor(a, entity)` | Whether `a` is above `entity` (parent, grandparent, ...) |
| `canParent(child, parent)` | Whether `setParent` would accept it |
| `worldMatrix(entity)` | Local-to-world matrix: the parents' matrices times the entity's own |
| `worldPosition(entity)` | The world position of the entity's origin |
| `parentWorldMatrix(entity)` | The parent's world matrix (identity for a root) |
| `setWorldMatrix(entity, m)` | Sets the local transform so the world matrix becomes `m` |
| `worldTransform(entity)` | The entity's transform in the world, as a `TransformComponent`: its own for a root, else its world matrix taken apart. 2D code reads a world `position2D()`, height (`position.z`), `yaw()` and `scale2D()` from it |
| `setWorldTransform(entity, t)` | Sets the local transform so the world transform becomes `t` (exactly, for a root) |

World matrices are worked out when asked for, by walking up the parents, so they
are never out of date. Everything that places an entity uses them, in every
style: meshes, sprites, point lights, the camera, particle emitters, camera
following, physics, picking and the editor's gizmo.

```cpp
// A 2D game: where a child is on the plane, wherever its parent has gone.
const sfs::TransformComponent world = sfs::hierarchy::worldTransform(child);
world.position2D(); // on the plane
world.yaw();        // turned, with its parents
```

A mirrored transform (a negative scale, as a sprite flipped to face the other
way) stays mirrored: taken apart, a mirrored matrix gives a negative x scale.

A parent doesn't need a `TransformComponent`. Without one it only groups its
children, which then sit in world space.

A transform can't hold shear, so `setParentKeepWorld` and `setWorldMatrix` can't
match a rotated child of a parent scaled differently along each axis exactly.
Uniform scales are exact.

## Walking the tree

`EntityTree` takes a snapshot of the structure for walking it top-down:

```cpp
const sfs::hierarchy::EntityTree tree(scene.entities());
for (const sfs::Entity& root : tree.roots())
  for (const sfs::Entity& child : tree.children(root))
    ...;

tree.preOrder();       // every entity, parents before children
tree.subtree(entity);  // entity and everything below it
```

Siblings keep the order of the list the tree was built from. A scene's
`entities()` is in id order.

## Destroying

`scene.destroyEntity` destroys only that entity. Its children become roots and
keep their local transforms, so they jump to those positions in world space.
`scene.destroyBranch(entity)` destroys the entity and everything below it.

Scenes can also find entities by name, or by a path of names from a root:
`scene.findEntity("Farm/Barn/Door")`.

## Saving

Scene files store the hierarchy as nesting: each entity lists its children
under `"children"`. See [Scene files](../../runtime/serialization/index.md).
