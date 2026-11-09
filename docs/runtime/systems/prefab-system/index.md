# Prefab system

`PrefabSystem` makes copies of prefab assets while the game runs, so a system
or a script can place things the way a scene file does: a forest planted from
its trees' prefabs, a crate dropped from a truck, a building put up by the
player. Every `StandardScene` has one.

```cpp
#include <sfs/sfs.h>

// From a system:
auto* prefabs = registry->tryGetSystem<sfs::PrefabSystem>();
// From a game object: scene().tryGetSystem<sfs::PrefabSystem>()

sfs::TransformComponent where;
where.position = {29.0f, 32.0f, 0.0f};
where.rotation = glm::angleAxis(0.4f, glm::vec3(0, 0, 1));
where.scale = glm::vec3(1.1f);

sfs::Entity tree = prefabs->spawn("asset:b87c181ec21487542704a4ebe5ed1339", where, group);
if (tree.isValid())
  tree.getComponent<sfs::MeshRendererComponent>().tint = {0.9f, 1.0f, 0.9f, 1.0f};

prefabs->despawn(tree); // it and everything below it, at the next flush
```

| Call | What it does |
| --- | --- |
| `spawn(prefab, transform, parent = {})` | Makes a copy of the prefab (`"asset:<id>"`: a `.prefab`, or a model's node tree) under `parent` (none: a root). `transform` replaces the prefab root's own, relative to the parent like every `TransformComponent`. Returns the copy's root |
| `despawn(root)` | Destroys the copy: its root, everything below it and their game objects. An invalid or already destroyed root is ignored |

## What a copy gets

A copy is made the same way a scene file's prefab instances are when the scene
loads (`SceneSerializer::instantiate`; see [Scene files](../../serialization/index.md#prefabs)):

- every entity and component in the prefab, nested prefabs and their overrides
  included, with the same `PrefabInstanceComponent` / `PrefabLinkComponent`
  markers;
- **colliders**: `PhysicsSystem3D` gives them bodies on its next update (a
  collider without a `RigidBody3DComponent` is static), and so they're in its
  `snapshot()`, which [navigation](../../navigation/index.md) plans against.
  Nothing else needs telling;
- its **game object scripts** (`ScriptComponent`) are made and their
  `onCreate` run, as when a scene starts playing.

Change the copy's components after `spawn` like any entity's (a tint, a level of
detail's settings); prefab data the copy keeps as it is costs nothing extra.

## Errors

`spawn` never throws. If the prefab can't be read (no such asset, not a prefab,
bad JSON) or `parent` has been destroyed, it logs an error, makes nothing and
returns an invalid entity: check `isValid()`.

## Cost

Prefabs are read once and kept by the asset store, so spawning many copies of
a few prefabs only costs the copies: 1000 spawns of a prefab with two entities
and a capsule collider take about **24 ms**, and their bodies about **10 ms**
on physics' next update (`prefabSystemTests`, Debug build). Spawning hundreds
at load is fine; spread thousands over frames if they would cost a frame.
