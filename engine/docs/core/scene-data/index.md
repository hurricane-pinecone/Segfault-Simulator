# Scene data internals

How scene data works inside the engine. What it's for and how a game uses it:
[Scene data](../../../../docs/core/ecs/index.md#scene-data).

The code is `engine/include/engine/core/ecs/sceneData.h` (traits, ids and the
compile checks), `engine/src/core/ecs/sceneData.cpp` (the id table), the
storage and calls in `registry.h`, `registry.inl` and `registry.cpp`, the
Service's calls in `service.inl`, and `Scene`'s in `scene.h` and `scene.cpp`.

## Marking

`struct [[sfs::scene_data]] Scoreboard` is read by sfs-codegen
(`AttributeId::SceneData`, `TypeKind::SceneData`). It's an error
(`scene-data-declaration`) on a component, an event, a type with a base, or an
unnamed, nested or template type. For each marked type codegen writes, into the
target's traits header (the one with the event traits, which every file of the
target includes first; the engine's own is `engineEventTraits.gen.h`, included
at the end of `sceneData.h` and `eventTraits.h`), a declaration of the type and

```cpp
template <>
struct SceneDataTraits<::game::Scoreboard>
{
  static constexpr bool marked = true;
};
```

Files scanned for traits alone (`--events-only`, `EVENT_SOURCES`) get them too,
so a test's `.cpp` or an engine header outside the reflected components can
declare scene data. A type in the reflected headers is also registered for
reflection, `sceneDataType<T>()` (registration.h), as `TypeKind::SceneData`
with `SceneDataOps` (get, add, remove, each given a Registry&).

## The compile checks

- `sceneData<T>()`, `addSceneData<T>()` and `removeSceneData<T>()` instantiate
  `detail::RequireSceneData<T>`, whose static_assert fails for an unmarked T.
- `Component<T>::getId()` instantiates `detail::NotAComponent<T>`, which fails
  for a marked T. Every component call (add, get, has, remove, query terms,
  registerComponent, observe, reflection's ComponentOps) asks for the id, so a
  marked type can't be used as a component anywhere.

## Storage and cost

- Each scene data type gets a slot id from its own table
  (`detail::sceneDataTypeId()`, engine-core, shared by every module as the
  component ids are), cached per module in a function-local static
  (`detail::sceneDataId<T>()`).
- `Registry::m_sceneData` is a dense vector of
  `std::unique_ptr<void, void (*)(void*)>`, indexed by slot id: the object on
  the heap and its deleter. It grows only when a type is first added (to that
  type's id), and each object is allocated once, by `addSceneData()`, so a
  pointer to it is stable until it's removed.
- `sceneData<T>()` is a bounds check and an index: no hashing, no search, no
  allocation. No per-frame cost for a scene without scene data.
- `sceneData<const T>()` reads the same slot as a `const T*` (the id is
  `const`-stripped): what every reader but the owner takes. Writing through
  it doesn't compile (ecsCompileFail case 18).
- `removeSceneData<T>()` destroys at once; `clearSceneData()` destroys all
  (the editor's `EditorScene::clear()`, before a snapshot is restored). The
  registry's destructor destroys what's left, after its services and systems.

## Scene files

`SceneSerializer::save()` writes `"sceneData": { "Type": { fields } }` when the
scene has any (never for a prefab's content), from the reflected scene data
types the scene holds (`Scene::sceneDataOf()`), each as `saveObject()` writes a
component's fields. `load()` reads it after the entities:

- An unregistered type is kept, verbatim, in the engine's own scene data
  `UnresolvedSceneData` (unresolvedComponents.h, not reflected) and written
  back by the next save, as `UnresolvedComponents` keeps an entity's.
- A registered type that isn't scene data is logged and kept the same way.
- A type the scene already holds is logged, and the scene's kept.
- A scene data type found as a component in an entity's entry is logged and
  kept on the entity's `UnresolvedComponents`, as the file had it.

A JSON object can't hold a key twice (the parser keeps the last), so a
duplicate key in a file isn't seen.

## The editor

Scene Settings > Scene Data lists the scene's scene data with the inspector's
field widgets, with Remove on each, and an Add Scene Data picker of the
registered types the scene hasn't got (in a prefab's tab, only a note that a
prefab holds none). Add Component lists components only, so
scene data never shows there. Edits go through the history like any scene
edit: a snapshot is a scene save, so it holds the scene data.
