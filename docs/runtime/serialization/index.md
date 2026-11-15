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

- **Every reflected component on every entity**, field by field. The engine's
  components are reflected (transforms, cameras, lights, sprites, 2D and 3D
  physics, particle emitters, grass, meshes...). A component that isn't
  reflected is left out.
- **The [hierarchy](../../core/hierarchy/index.md)**, as nesting: an entity's
  children are listed under its `"children"` key, beside its components. A
  component can't be called `children`.
- **The scene's render settings**, when it has a `RenderSystem`: its
  [style](#the-style), lighting, shadows, wind, haze and the camera. In an editor scene, that camera is the viewport's
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
      "TransformComponent": {
        "position": [-2.0, 1.0, 0.0],
        "rotation": [1.0, 0.0, 0.0, 0.0],
        "scale": [1.0, 1.0, 2.0]
      },
      "children": [
        {
          "NameComponent": { "name": "Lid" },
          "TransformComponent": { "position": [0.0, 0.0, 1.0], ... }
        }
      ]
    }
  ],
  "formatVersion": 3,
  "render": { "style": { "kind": "3d" }, "camera": {}, "lighting": {}, "shadows": {} }
}
```

Quaternions are stored as `[w, x, y, z]`. A child's transform is relative to
its parent.

Format 1 files, from before hierarchies, are the same without `"children"`, and
still load. Format 2 files are format 3 with models' nodes named by their old
ids (see [Model node ids](#model-node-ids)), and still load.

## The style

`"render"` → `"style"` says how the scene is drawn (see
[Styles](../rendering/styles/index.md)):

```json
"style": { "kind": "flat", "pixelsPerUnit": 1, "tilePixels": [32, 16],
           "heightPixels": 8, "yDown": true, "ySort": false }
```

`"kind"` is `"3d"`, `"flat"` or `"isometric"`. For a 2D kind the other keys are
the scene's `Space2D`; loading gives the `RenderSystem` that `Space2D`
(`setSpace2D`), and `"3d"` clears it. A file without `"style"`, from before
there were styles, loads as 3D. `StandardScene` plays a file in any style.

## Saving part of a scene

`saveEntity(entity)` saves one entity and everything below it, in the
same form as an entry in `"entities"`. `loadEntity(entry, scene, parent)`
creates it again, under `parent` or as a root. The editor duplicates a branch
this way:

```cpp
sfs::Entity copy = serializer.loadEntity(serializer.saveEntity(lamp),
                                         scene,
                                         sfs::hierarchy::parentOf(lamp));
```

The copy's [references](#entity-references) to entities inside the branch name
the copy's own entities, and those to entities outside it the same ones as the
original's.

To load a scene file's entries a few at a time (the editor opens a big scene
over several frames), pass each `loadEntity` call a
`SceneSerializer::LoadSession` and call `finish(session, scene)` once every
entry is in: references between entries wait for it.

## Entity references

A reflected `sfs::Entity` field (or `std::vector<sfs::Entity>`, see
[Reflection](../../core/reflection/index.md#field-types)) points at another
entity. Entity ids change from load to load, so it's saved as a path, `null`
for none, and resolved once the load has made every entity:

| Saved as | Names | Where |
| --- | --- | --- |
| `"1"`, `"1/2318043377"` | the entity at that `$id` path (as `$overrides` keys) from the context root | prefab files, instance entries |
| `""` | the context root itself | as above |
| `"../3"` | up one path segment, then down | a nested instance's entry in a prefab file naming the outer prefab's entity |
| `"/7"`, `"/7/1/2318043377"` | the scene entry with `"$sceneId": 7`, then a path in its instance | scene files |
| `"#2"`, `"#2/1"` | the third entity loading the copy makes as an entry (from `#0`, the branch's top: plain entities, instance roots and what was added to an instance), then a path in its instance | `saveEntity` output only |

The **context root** is the prefab's root for a prefab file's own content, and
an instance's root for what its entry holds (`$overrides`, `$added`), so each
instance of a prefab resolves to its own parts. A scene's plain entries have
none: their references are scene-absolute. For example:

```json
// Tractor.prefab, the root's components: its model instance is $id 1
"Linkage": { "lowerLeft": "1/2318043377", "arms": ["1/3051889210", "1/2694101533"] }
// a scene: one entry's weighbridge is another entry
{ "SellPoint": { "weighbridge": "/7" } }    { "$sceneId": 7, "NameComponent": { "name": "Weighbridge" } }
```

- Paths use the engine's own ids, never names: renaming or moving an entity in
  the editor changes nothing, and neither does reordering a model's nodes (see
  [Model node ids](#model-node-ids)).
- **`$sceneId`** (a hidden `SceneIdComponent`) is given only to the entries a
  reference names, when the scene is saved or a branch copied (`saveEntity`),
  and kept from then on: existing ids are never reused or renumbered. A new
  one is a random 31-bit number not already in use in the scene, as Unity's
  file ids are, so two branches of a scene that each add a referenced entity
  give them different ids and merge cleanly. The ids come from the engine's
  random source (`core/util/random.h`); `random::seed` makes them repeat, for
  tests. A scene without references saves exactly as before.
- Reading an instance's entry (`instanceEntry`, as the editor does to show
  its overrides) changes nothing: it gives no scene ids, so a reference whose
  target has none yet is written as `null` there.
- A reference to an entity outside the prefab is saved as `null`, with a
  warning: a prefab can't point outside itself. An entity a prefab adds
  inside one of its nested instances is its own, named through that instance
  by its added id (`"5/1535798911"`).
- An unchanged reference in an instance writes no override, and a changed one
  writes one, like any other field.
- A reference whose target isn't there when it loads (a model node renamed in
  its source file, say, or a `..` above the prefab's root) is logged, once
  per prefab, field and value however often the prefab is spawned, and left
  empty. Its saved value is kept (`UnresolvedComponents::refs`) and saved back
  while the field stays empty; setting the field drops it.
- An integer, as some fields held entity ids before references were paths,
  loads as none, with a warning (`-1`, none, without one).
- Nothing is looked up while the game runs: a resolved reference is a field,
  read like any other `sfs::Entity` (check it, `if (entity)`, before using it).
- For tools (the editor): `entryRootOf(entity)` is the instance whose entry
  holds an entity's components (its references' context root in a scene),
  `sceneHolderOf(target)` the entry a scene-absolute reference names a target
  through and its path there, `sceneIdHolders(scene)` the entities by scene
  id, `entityAtPath(root, path)` the entity a path names,
  `keptReference`/`keptReferences` the saved values kept for empty references,
  and `jsonText(value, depth)` a value as files write it (to edit one in place
  with `findJsonValue` and `applyTextEdits` from `core/util/jsonTokens.h`).

## Prefabs

A prefab is a saved branch of entities that scenes place copies of. A
`.prefab` file holds one entry, in the scene-file form, with a `"$id"` on each
entity below the root (unique in the file):

```json
{ "formatVersion": 2,
  "root": { "NameComponent": { "name": "Fence" }, "TransformComponent": { ... },
            "children": [ { "$id": 1, "MeshRendererComponent": { ... }, ... } ] } }
```

A copy (an *instance*) is saved as the prefab and how it differs from it:

```json
{ "$prefab": "asset:99f82e199edf433a8766a309dc98d973",
  "$overrides": { "": { "TransformComponent": { "position": [3, 1, 0] } },
                  "1": { "MeshRendererComponent": { "tint": [1, 0, 0, 1] } } },
  "$removed": [ "2" ],
  "$added": [ { "$parent": "", "NameComponent": { "name": "Lamp" }, ... } ] }
```

- **Paths** name an instance's entities by their `$id`: `""` is its root, and
  `"3/7"` is entity 7 of the prefab placed as entity 3 (prefabs nest).
- **What a prefab adds inside a nested instance** (the `$added` of a nested
  instance's entry in a `.prefab`) belongs to that prefab. Each such entity
  has a `$id` with bit 30 set (`kAddedNodeIdBit`; made from its place in the
  entry when an older file has none, the same at every load), and its path
  runs through the nested instance like that instance's own entities
  (`"3/1073741907"`). So every copy of the prefab knows it as its own: an
  unchanged copy saves as just the prefab, and a changed one as overrides.
- **Overrides** are JSON merge patches of an entity's components: only the
  fields that differ (`null` removes a component). They're worked out when
  saving, by comparing each entity with what the prefab alone gives, so any
  change is kept; editing the prefab changes every copy except where a copy
  overrides it.
- **A model is a prefab too**: `$prefab` naming a model asset places its node
  tree, each node an entity with its part. The asset store builds that prefab
  with `"model": true`, so every entity it makes (its root included) gets a
  `RestPoseComponent`: its transform as the instance was built, after every
  override (the outer prefab's and the scene's), before anything moved it.
  Code that turns or moves a part from its rest reads it
  (`transform.rotation = turn * rest.rotation`) instead of keeping its own
  copy. It isn't reflected, so it's never saved or shown. A `.prefab`'s own
  entities don't get one. Pressing Play in the editor records it again for
  every entity that has one, as the scene is then, since the scene plays in
  place rather than reloading; `hierarchy::recordRestPose(entity)` records one
  from code.
- An instance whose prefab can't be read loads as a placeholder and is saved
  back unchanged; a prefab containing itself is left out (logged). Only a
  prefab's own content can make a cycle: a copy of a prefab added (`$added`)
  under a copy of the same prefab in a scene, or in another prefab, is no
  cycle and loads.
- A prefab's content opened on its own (the editor's prefab tab: its root no
  longer an instance) keeps its entities' ids when the scene holding it is
  saved and loaded again (an undo snapshot): `save(scene, systems, true)`
  writes each plain entry's `PrefabLinkComponent` as `"$id"`, and loading a
  plain entry with one gives it back. Other saves write none, so scenes save
  as before.
- An override, `$removed` path or `$added` entity's `$parent` that names an
  entity the prefab no longer has (a model node renamed or deleted in its
  source file) is kept, never dropped: it's held, unused, in the instance
  root's hidden `UnresolvedComponents` (its `overrides`), logged once when the
  instance loads, and saved back unchanged. It applies again if the entity
  comes back.

### Model node ids

A model's prefab gives each node a stable `$id`: a hash of its *name path*,
the node's names from the model's top node down to it, joined with `/`
(`"body/wheel-front-left"`; the model file's own name isn't part of it). So
reordering the nodes in the source file, or moving or renaming the file,
changes no id, and every override stays on its node. Renaming a node gives it
and the nodes below it new ids; their old overrides are then kept unused (see
above).

- Ids have bit 31 set (`kModelNodeIdBit`, e.g. `2554517965`), so they never
  meet the small ids a `.prefab` gives its own entities, and `savePrefab` never
  counts on from one.
- Two siblings with the same name are `name`, `name#1`, `name#2`... in the
  file's order, and unnamed nodes are named `""` the same way (shown as
  `Node <n>`), so their ids follow their order (the import logs a warning: give
  them unique names in the source file). A `/`, `#` or `\` in a name is
  escaped with a `\`.
- If two name paths of one model hash to the same id, the later one in the
  file takes the next free id (logged), the same way every time.
- Names are read as their JSON text means them (`"caf\u00e9"` is `café`), so
  an id doesn't depend on how the exporter spelled a name.
- `stableNodeId(namePath)` (`modelImporter.h`) computes an id;
  `importModelNodes(file, settings, &namePaths)` gives each node's name path.

Older files (prefab format 1, scene format 2) named a model's nodes by their
place in the model file's node order (`1`, `2`...). A path into a model with
such a number (below bit 30) is read as the node in that place, so older
files, autosaves and other branches load onto the same nodes. Saving writes the
stable ids. A number past the model's nodes is read and saved as `old:<n>`, so
it stays unused and can't name a node the model gains later. To upgrade a
project's files in one go, without opening them:

```
SfsEditor --upgrade-node-ids <project-folder> [--check]
SfsEditor --list-node-ids <model-file>
```

`--upgrade-node-ids` edits only the old id strings (and `formatVersion`) of
every prefab and scene that has any, leaving every other byte as it was, and
leaves the other files alone. Before it writes anything it checks that every
instance's prefab or model can be read, that each edited file parses to exactly
the upgrade of the old one, that each edited path names a node with the same
name path by its old id as by its new one, that every entry makes the same
entities from the old files as from the upgraded ones, and that a second run
would change nothing. If any check fails it writes nothing (and exits
non-zero). It stages every upgraded file beside its own before replacing any,
and puts the replaced ones back if a later one can't be written. `--check`
runs the checks and reports what it would change.
`SceneSerializer::nodeIdEdits` gives the edits for one file, and
`scanJsonTokens` (`core/util/jsonTokens.h`) each key's and value's JSON pointer
and byte range, so a tool can change just those bytes (`applyTextEdits`).
`--list-node-ids` prints a model's nodes with their old and stable ids and
name paths.

`SceneSerializer::instantiate`, `savePrefab` and `instanceEntry` do this from
code; `setPrefabSource` says where prefabs come from (`StandardScene::serializer()`
reads them through the asset store); while a game runs, systems and scripts
make copies with [`PrefabSystem::spawn`](../systems/prefab-system/index.md).
At runtime an instance's root has a
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
| Component saved under a retired engine name (`Transform3DComponent`, `Camera3DComponent`, `PointLight3DComponent`) | Loads as the current one (`TransformComponent`, `CameraComponent`, `PointLightComponent`) |
| A retired sprite component (`SpriteTint`, `ShadowCasterComponent`) | Folded into the entity's `SpriteComponent` (its `tint`, `castsShadow`); dropped if it has none |
| Component type not registered (renamed, removed, or its module not built yet) | Its data is kept on the entity and written back unchanged on save; it loads as a real component once the type exists |
| Mesh name not found | A stand-in that keeps the name and draws nothing (an error is logged); saving writes the name back, and it loads as the real mesh once one has that name |
| Model nodes named by their old ids (place in the model file) | Read as the nodes in those places; saved back with their stable ids |
| Override or path naming an entity the prefab no longer has | Kept, unused, and saved back unchanged; a warning is logged |
| Entity reference naming an entity that isn't there | Left empty, and saved back unchanged while it stays so; a warning is logged |
| Entity reference saved as an entity id (an integer) | None; a warning is logged |
| File from a newer format | Refused |
