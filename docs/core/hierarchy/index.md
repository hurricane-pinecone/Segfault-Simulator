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
| `setParent(child, parent)` | Puts `child` under `parent`, keeping its local transform, so it moves to sit relative to the parent. A default-made or destroyed `parent` makes it a root. It takes its place among the parent's children in id order. Returns false, changing nothing, if `canParent` refuses it: a loop, a destroyed child given any parent but a default-made one, or a parent (living or destroyed) in another registry (the last two logged). Making a destroyed entity a root with a default-made parent does nothing and returns true. |
| `setParentKeepWorld(child, parent)` | The same, but the child stays where it is in the world: its local transform is recalculated. This is what the editor does when you drag in the Hierarchy. |
| `parentOf(entity)` | The parent, or an invalid entity for a root (or a destroyed or stale handle) |
| `isAncestor(a, entity)` | Whether `a` is above `entity` (parent, grandparent, ...) |
| `rootOf(entity)` | The top of its branch: its root ancestor, or itself for a root |
| `canParent(child, parent)` | Whether `setParent` would accept it: always for a default-made parent (a root); otherwise the child must be alive and in the parent's registry, and a living parent must not be the child or below it (a destroyed one makes it a root) |
| `childrenOf(entity)` | The children, in id order, as a range of `Entity` |
| `childrenOf<Terms...>(entity)` | The children that match the terms (as a query matches them: components, one `Any` group, `Excluding` groups), in id order, as rows that unpack their components like a query's |
| `hasChildren(entity)` | Whether it has any |
| `findChild<Terms...>(entity)` | The first child that matches the terms (`findChild<Tag>(entity)`), or an invalid entity |
| `findChildren<Terms...>(entity, out)` | Every child that matches the terms, into `out` (cleared first) |
| `descendants(root)` | Everything below `root` (not `root` itself), parents first |
| `descendants<Terms...>(root)` | The entities below `root` that match the terms, parents first, as rows |
| `findDescendant<Terms...>(root)` | The first entity below `root`, parents first, that matches the terms |
| `roots(registry)` | The entities with no parent, in id order (`scene.roots()` from a scene) |
| `preorder(registry)` | Every entity, parents first, with its parent's place in the list (`scene.preorder()` from a scene) |
| `worldMatrix(entity)` | Local-to-world matrix: the parents' matrices times the entity's own. Always fresh (see [World matrices](#world-matrices)) |
| `cachedWorldMatrix(entity)` | The world matrix the transform pass cached this frame: for the render phase only |
| `cachedWorldTransform(entity)` | `worldTransform` from the cache: for the render phase only |
| `composeWorld(parentWorld, local)` | One level: a parent's world matrix times a local transform's, as 3x4 affine matrices. Every world matrix is composed through it |
| `worldPosition(entity)` | The world position of the entity's origin |
| `parentWorldMatrix(entity)` | The parent's world matrix (identity for a root) |
| `setWorldMatrix(entity, m)` | Sets the local transform so the world matrix becomes `m` |
| `worldTransform(entity)` | The entity's transform in the world, as a `TransformComponent`: its own for a root, else its world matrix taken apart. 2D code reads a world `position2D()`, height (`position.z`), `yaw()` and `scale2D()` from it |
| `setWorldTransform(entity, t)` | Sets the local transform so the world transform becomes `t` (exactly, for a root) |

Everything that places an entity uses world matrices, in every style: meshes,
sprites, point lights, the camera, particle emitters, camera following,
physics, picking and the editor's gizmo. `worldMatrix` is never out of date
(see [World matrices](#world-matrices) for how it stays cheap).

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

## World matrices

Every scene has a `TransformSystem`, added first, and its `render()` opens the
render phase: after every update, game object update and late update, before
anything is drawn. It's the *transform pass*. Once a frame it walks the
preorder index from the roots down and keeps each entity's world matrix in a
`WorldTransformComponent`, which it gives every entity with a
`TransformComponent` (and takes away with it). The component is hidden: never
saved, shown or added by hand.

```cpp
struct WorldTransformComponent // 64 bytes
{
  glm::mat4x3 world;       // affine: the scaled x, y and z axes, then the origin
  uint64_t localHash;      // transformHash() of the local transform it was made from
  uint32_t version;        // the pass that last changed it; 0: not made yet
  Entity::EntityId parent; // the nearest ancestor with a transform it was made under
};
```

The pass recomposes only what changed: an entity whose local transform's hash,
nearest ancestor with a transform, or that ancestor's matrix changed since the
last pass. Each cache it changes gets the pass's number as its `version`, so
code that wants to know what moved compares versions. Nothing tells the engine
when game code writes a transform, so the pass hashes each one (40 bytes, two
multiply-xorshift chains) instead of keeping a copy to compare with. A change
within one 64-bit word of a transform can never hash the same; a change across
words does at about 2^-64, and would then go unseen until that entity or an
ancestor is written again (the transform check below catches it).

Every world matrix is composed from the top down by one function,
`composeWorld`, so a cached matrix and one composed afresh are equal to the
bit whenever nothing changed.

**Reading them:**

- `worldMatrix(entity)` (and `worldPosition`, `worldTransform`,
  `parentWorldMatrix`, `setWorldMatrix`, `setWorldTransform`) is always fresh,
  wherever it's called. It walks up the chain and checks each level's cache: a
  level is clean when its cache was made from its transform as it is now, under
  the same ancestor. Above the highest level that isn't, the caches are exactly
  what composing would give, so it composes only from there down. Move a
  parent and its children's next `worldMatrix` follows; a chain nothing has
  touched since the pass comes straight from the cache, with no multiply.
- `cachedWorldMatrix(entity)` reads the cache as the pass left it, with no
  check. It's for code that runs after the pass in the same frame: the render
  phase (`RenderSystem` and its modules). Anywhere else a transform written
  since the pass wouldn't be seen. An entity made since the last flush has no
  cache yet (nor do the entities below it until the next pass after the
  flush): it's read through `worldMatrix` instead.

Nothing in the render phase may write a `TransformComponent`, or what's drawn
is a frame behind. The engine's render code writes none.

**The transform check.** `SFS_CHECK_TRANSFORMS=1` in the environment (a debug
feature, so in the editor, Debug builds and development exports only) checks
every cache against a world matrix composed afresh, after each pass, and every
cached read in the render phase. A mismatch logs one warning per entity
("SFS_CHECK_TRANSFORMS: entity 12 'Plough': its cached world matrix, read in
the render phase, differs from one composed afresh ..."). It catches a
transform written after the pass and any bug in the cache, at the cost of a
full recompute every frame. Tests turn it on with
`debugFeatures::setCheckTransforms(true)`.

## Walking the tree

The walks read the registry's links where they are, and allocate nothing:

```cpp
for (const sfs::Entity& wheel : sfs::hierarchy::childrenOf(tractor))
  ...;

// The children with a Wheel: a row of one component converts to it, as a
// query's does; `auto [wheel, spin]` unpacks the entity too.
for (Wheel& spin : sfs::hierarchy::childrenOf<Wheel>(tractor))
  spin.angle += spin.speed * dt;

// A part by its tag component, one level down or anywhere below. A finder
// takes several terms, as a query does.
const sfs::Entity cab = sfs::hierarchy::findChild<Cab>(tractor);
const sfs::Entity hitch = sfs::hierarchy::findDescendant<HitchPoint>(tractor);
const sfs::Entity lamp = sfs::hierarchy::findChild<Lamp, sfs::Excluding<Broken>>(tractor);
if (auto* animator = sfs::hierarchy::findChild<sfs::AnimatorComponent>(hand)
                         .getComponent<sfs::AnimatorComponent>())
  animator->play("wave");

// Into a buffer you keep, so walking again allocates nothing.
sfs::hierarchy::findChildren<Wheel>(tractor, m_wheels);

// Everything below, parents first, leaving out what's below a nested machine.
const sfs::hierarchy::DescendantRange parts = sfs::hierarchy::descendants(tractor);
for (auto it = parts.begin(); it != parts.end(); ++it)
  if ((*it).hasComponent<Machine>())
    it.skipChildren(); // its parts are its own
```

The typed walks, `childrenOf<Terms...>()` and `descendants<Terms...>()`, test
each entity's components against the terms and yield the matching ones as
query rows (see [Rows](../ecs/index.md#rows-unpacking-the-components)):
required components bound by reference, `Any` members by pointer, a row of
one required component converting to it, and `components()` giving the rows
without their entity (a `descendants` walk's then has no `skipChildren()`).
Unlike a query they include entities still waiting for their first flush, as
`childrenOf()` does. A row's entity is valid for its loop step. In a typed
`descendants` walk, the iterator's `skipChildren()` leaves out what is below
the current row's entity; entities that don't match are walked through, so to
prune at those, take the untyped walk. A finder of one entity
(`findChild`, `findDescendant`) gives an `Entity`: look a component up on it
(`findChild<T>(e).getComponent<T>()` is null-safe, as an invalid handle gives
null).

Siblings are always in id order. That is the order a scene was loaded in, and
the order the editor's Hierarchy shows and scenes save in. A destroyed or stale
handle has no parent and no children.

To reparent the entities you're walking, or add or remove the components a
typed walk names, walk a copy: the walk follows the links as they are at each
step. Destroying while walking is fine, since it waits for the flush.

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
| `isAncestor`, `rootOf` | O(depth) array reads |
| `setParent` | O(depth) for the loop check, then O(1) to relink (appending in id order is the usual case; a reused lower id walks back from the last child) |
| Destroying | O(1) plus O(children) at the flush |
| `childrenOf`, `findChild`, `findChildren` | O(children); typed, a signature test per child plus the components a row binds |
| `descendants`, `findDescendant` | O(entities visited), no stack; typed, a signature test per entity plus the components a row binds |
| `roots` | O(entity ids) for a whole walk |
| `preorder` | A read, or O(entities) after a change |
| `worldMatrix` | Per level up the chain: the parent link and the transform; then, from the top down to the highest changed level, the cache and a hash of the transform; then one `composeWorld` per level below it |
| `cachedWorldMatrix` | A read |
| The transform pass | Once a frame: a hash per entity with a transform, plus a `composeWorld` for each changed one; nothing allocated |
| Memory | 16 bytes of links per entity id, 8 more per entity for the preorder index; 72 bytes per entity with a transform for its world cache (in its pool slot), and 16 bytes per entity for the pass's scratch |

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
