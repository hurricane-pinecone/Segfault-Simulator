# Scene files

`SceneSerializer` saves a scene to JSON and loads it back. It needs no
per-component code: it walks the [reflected](../../core/reflection/index.md)
components of every entity, so any component you write is saved automatically.

```cpp
#include "engine/core/reflection/reflection.h"
#include "engine/runtime/serialization/sceneSerializer.h"

sfs::SceneSerializer serializer(sfs::reflect::types(), &renderSystem.meshes());

serializer.saveFile(scene, "scenes/main.scene");
serializer.loadFile("scenes/main.scene", scene); // adds to the scene's entities
```

`load` adds entities to the scene; it doesn't clear what is already there. To
replace a scene's contents, destroy its entities and call
`scene.flushEntities()` first.

## What is saved

- **Every reflected component on every entity**, field by field. Components
  that aren't reflected, such as most of the 2D/isometric ones, are left out.
- **The [hierarchy](../../core/hierarchy/index.md)**, as nesting: an entity's
  children are listed under its `"children"` key, beside its components. A
  component can't be called `children`.
- **The scene's render settings**, when it has a `RenderSystem3D`: lighting,
  shadows and the camera. In an editor scene, that camera is the viewport's
  view. The game sees through a camera entity, which is saved like any other
  entity.
- **The scene's systems** (the `System` scripts it runs when it plays, and
  the engine's optional systems such as `PhysicsSystem3D`), by name, under
  `"systems"`. Pass the list to `save` and get it back from
  `load`; nothing instantiates them until the scene plays.
- **Meshes by name.** A `MeshHandle` is only valid for one run, so a mesh field
  is saved as the name the mesh was given in `MeshLibrary::add(mesh, name)`.
  Name every mesh that a saved entity uses. Saving a mesh with no name logs an
  error.

```json
{
  "entities": [
    {
      "MeshRendererComponent": {
        "castsShadow": true,
        "mesh": "builtin:cube",
        "tint": [0.9, 0.25, 0.2, 1.0]
      },
      "NameComponent": {
        "name": "Red Box"
      },
      "Transform3DComponent": {
        "position": [-2.0, 1.0, 0.0],
        "rotation": [1.0, 0.0, 0.0, 0.0],
        "scale": [1.0, 1.0, 2.0]
      },
      "children": [
        {
          "NameComponent": { "name": "Lid" },
          "Transform3DComponent": { "position": [0.0, 0.0, 1.0], ... }
        }
      ]
    }
  ],
  "formatVersion": 2,
  "render": { "camera": {}, "lighting": {}, "shadows": {} }
}
```

Quaternions are stored as `[w, x, y, z]`. A child's transform is relative to
its parent.

Format 1 files, from before hierarchies, are the same without `"children"`, and
still load.

## Saving part of a scene

`saveEntity(scene, entity)` saves one entity and everything below it, in the
same form as an entry in `"entities"`. `loadEntity(entry, scene, parent)`
creates it again, under `parent` or as a root. The editor duplicates a branch
this way:

```cpp
sfs::Entity copy = serializer.loadEntity(serializer.saveEntity(scene, lamp),
                                         scene,
                                         sfs::hierarchy::parentOf(lamp));
```

## Prefabs

A prefab is a saved branch of entities that scenes place copies of. A
`.prefab` file holds one entry, in the scene-file form, with a `"$id"` on each
entity below the root (unique in the file):

```json
{ "formatVersion": 1,
  "root": { "NameComponent": { "name": "Fence" }, "Transform3DComponent": { ... },
            "children": [ { "$id": 1, "MeshRendererComponent": { ... }, ... } ] } }
```

A copy (an *instance*) is saved as the prefab and how it differs from it:

```json
{ "$prefab": "asset:99f82e199edf433a8766a309dc98d973",
  "$overrides": { "": { "Transform3DComponent": { "position": [3, 1, 0] } },
                  "1": { "MeshRendererComponent": { "tint": [1, 0, 0, 1] } } },
  "$removed": [ "2" ],
  "$added": [ { "$parent": "", "NameComponent": { "name": "Lamp" }, ... } ] }
```

- **Paths** name an instance's entities by their `$id`: `""` is its root, and
  `"3/7"` is entity 7 of the prefab placed as entity 3 (prefabs nest).
- **Overrides** are JSON merge patches of an entity's components: only the
  fields that differ (`null` removes a component). They're worked out when
  saving, by comparing each entity with what the prefab alone gives, so any
  change is kept; editing the prefab changes every copy except where a copy
  overrides it.
- **A model is a prefab too**: `$prefab` naming a model asset places its node
  tree, each node an entity with its part.
- An instance whose prefab can't be read loads as a placeholder and is saved
  back unchanged; a prefab containing itself is left out (logged).

`SceneSerializer::instantiate`, `savePrefab` and `instanceEntry` do this from
code; `setPrefabSource` says where prefabs come from (`Scene3D::serializer()`
reads them through the asset store). At runtime an instance's root has a
`PrefabInstanceComponent`, and each entity from a prefab a
`PrefabLinkComponent` with its `$id`.

## Version control

Scene files are designed to be committed:

- Saving, loading and saving again produces a byte-identical file.
- Entities keep their order. They're saved parents first, siblings in id order,
  and loading creates them in that order.
- Floats are written as their shortest exact decimal (`0.45`, not
  `0.44999998807907104`).
- Vectors and colours stay on one line.

## When code changes

A scene still loads after the code it was saved with has changed:

| Change | On load |
| --- | --- |
| Field added to a component | Keeps its default value |
| Field removed or renamed | Ignored |
| Field's type changed | Keeps its default; an error is logged |
| Component type not registered (renamed, removed, or its module not built yet) | Its data is kept on the entity and written back unchanged on save; it loads as a real component once the type exists |
| Mesh name not found | A stand-in that keeps the name and draws nothing (an error is logged); saving writes the name back, and it loads as the real mesh once one has that name |
| File from a newer format | Refused |
