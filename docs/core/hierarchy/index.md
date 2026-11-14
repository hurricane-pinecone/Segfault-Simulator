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

The registry keeps the links, both ways: each entity's parent, first child and
next and previous siblings, by entity id. There's no component to write, so
`hierarchy::setParent` is the one way to change them, and it refuses to make a
loop (an entity under its own child). A change shows at once: `childrenOf`
lists a new child straight away, before any flush.

## Functions

All are in `sfs::hierarchy`.

| Function | Does |
| --- | --- |
| `setParent(child, parent)` | Puts `child` under `parent`, keeping its local transform, so it moves to sit relative to the parent. An invalid or destroyed `parent` makes it a root. It takes its place among the parent's children in id order. Returns false, changing nothing, if `canParent` refuses it: a loop, a destroyed child given a valid parent, or a parent in another registry (the last two logged). Making a destroyed entity a root does nothing and returns true. |
| `setParentKeepWorld(child, parent)` | The same, but the child stays where it is in the world: its local transform is recalculated. This is what the editor does when you drag in the Hierarchy. |
| `parentOf(entity)` | The parent, or an invalid entity for a root (or a destroyed or stale handle) |
| `isAncestor(a, entity)` | Whether `a` is above `entity` (parent, grandparent, ...) |
| `canParent(child, parent)` | Whether `setParent` would accept it: always for an invalid parent (a root); otherwise the child must be alive, in the parent's registry, and not the parent or above it |
| `childrenOf(entity)` | The children, in id order, as a range of `Entity` |
| `hasChildren(entity)` | Whether it has any |
| `findChild<Tag>(entity)` | The first child with a `Tag` component, or an invalid entity |
| `findChildren<Tag>(entity, out)` | Every child with a `Tag` component, into `out` (cleared first) |
| `descendants(root)` | Everything below `root` (not `root` itself), parents first |
| `findDescendant<Tag>(root)` | The first entity below `root`, parents first, with a `Tag` component |
| `roots(registry)` | The entities with no parent, in id order (`scene.roots()` from a scene) |
| `preorder(registry)` | Every entity, parents first, with its parent's place in the list (`scene.preorder()` from a scene) |
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

The walks read the registry's links where they are, and allocate nothing:

```cpp
for (const sfs::Entity& wheel : sfs::hierarchy::childrenOf(tractor))
  ...;

// A part by its tag component, one level down or anywhere below.
const sfs::Entity cab = sfs::hierarchy::findChild<Cab>(tractor);
const sfs::Entity hitch = sfs::hierarchy::findDescendant<HitchPoint>(tractor);

// Into a buffer you keep, so walking again allocates nothing.
sfs::hierarchy::findChildren<Wheel>(tractor, m_wheels);

// Everything below, parents first, leaving out what's below a nested machine.
const sfs::hierarchy::DescendantRange parts = sfs::hierarchy::descendants(tractor);
for (auto it = parts.begin(); it != parts.end(); ++it)
  if ((*it).hasComponent<Machine>())
    it.skipChildren(); // its parts are its own
```

Siblings are always in id order. That is the order a scene was loaded in, and
the order the editor's Hierarchy shows and scenes save in. A destroyed or stale
handle has no parent and no children.

To reparent the entities you're walking, walk a copy: the walk follows the
links as they are at each step. Destroying while walking is fine, since it
waits for the flush.

`preorder(registry)` (or `scene.preorder()`) lists every entity, parents
before children, roots in id order, each entry with its id and its parent's
place in the list (`HierarchyLinks::kNone` for a root). The registry rebuilds
it only when it's asked for after a link changed or an entity was created or
removed; otherwise it's a read. The rebuild rewrites the list in place, so
while you iterate it, call nothing that may call `preorder()` itself (saving
a scene, `findEntity`). `roots(registry)` (or `scene.roots()`) walks just the
roots, reading the links as it goes, so it's never stale and creating
entities while walking it is safe.

The hierarchy is main-thread only: the lazy rebuild of the preorder writes on
what looks like a read.

### Costs

| | Cost |
| --- | --- |
| `parentOf`, `hasChildren` | One array read after the alive check |
| `setParent` | O(depth) for the loop check, then O(1) to relink (appending in id order is the usual case; a reused lower id walks back from the last child) |
| Destroying | O(1) plus O(children) at the flush |
| `childrenOf`, `findChild`, `findChildren` | O(children) |
| `descendants`, `findDescendant` | O(entities visited), no stack |
| `roots` | O(entity ids) for a whole walk |
| `preorder` | A read, or O(entities) after a change |
| Memory | 16 bytes of links per entity id, 8 more per entity for the preorder index |

## Destroying

`scene.destroyEntity` destroys only that entity. At the flush, its children
become roots and keep their local transforms, so they jump to those positions
in world space. `scene.destroyBranch(entity)` destroys the entity and
everything below it.

Scenes can also find entities by name, or by a path of names from a root:
`scene.findEntity("Farm/Barn/Door")`.

## Saving

Scene files store the hierarchy as nesting: each entity lists its children
under `"children"`, in id order, roots first. See
[Scene files](../../runtime/serialization/index.md).
