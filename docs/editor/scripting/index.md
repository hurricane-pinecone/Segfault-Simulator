# Game scripts

Gameplay is native C++. You write game objects, systems and components as
ordinary classes. The editor compiles them into your project's **game module**
and runs them when you press **Play**. There are no macros, no registration
calls and no scripting language.

## Creating a script

Choose **Scripts → New GameObject Script / New System / New Component**, and
name it. The editor writes `scripts/<Name>.h` from a template and opens it in
the [Code panel](../index.md#code-panel):

```cpp
#pragma once
#include <sfs/sfs.h>

class Player : public sfs::GameObject
{
protected:
  void onCreate(sfs::Scene& scene) override
  {
  }

  void onUpdate(double deltaTime) override
  {
  }
};
```

The class must be named after its file. The engine finds scripts by that name
and by the class's base, as described in
[Reflection & codegen](../../core/reflection/index.md). You can also add `.cpp`
files to `scripts/`; they're compiled into the module.

| Kind | Base | Runs |
| --- | --- | --- |
| GameObject script | `sfs::GameObject` | On the entity it's attached to: `onCreate`, then `onUpdate` every frame; `onProcessInput` gets the keyboard and mouse |
| System | `sfs::System` | Every frame, over each entity that has its registered components |
| Component | none | Nothing; plain data on an entity, edited in the inspector |
| Mesh source | `sfs::MeshSource` | When a scene is created: adds meshes built in code, by name (see below) |

## Using scripts in a scene

- **Game object scripts:** select an entity, choose **Add Component → Script**,
  and pick the class from the dropdown. When the scene plays, an instance of
  that class is created bound to the entity, so its `entity()` is that entity.
- **Systems:** in **Scene Settings → Game Systems**, choose **Add System**.
  The scene saves the list, and the systems are added when it plays.
- **Components:** select an entity and choose **Add Component**. Your
  components are listed with the engine's.

Your scripts appear in these lists once they're built (**Ctrl+B** or **Play**).

### Script fields

A game object script's public fields, and private ones marked
`[[sfs::show]]`, are its settings. They appear in the Inspector under the
Script component and are saved **per entity**, so two entities running the
same script can be tuned differently:

```cpp
class Spinner : public sfs::GameObject
{
public:
  [[sfs::range(0, 10)]] float turnsPerSecond = 0.25f;
  glm::vec3 axis{0.0f, 0.0f, 1.0f};
  bool reverse = false;
  ...
};
```

The values in the class are the defaults. An entity's values are applied
before its `onCreate`, so `onCreate` already sees them. Edits are undoable,
like any other. The Inspector shows the fields once the scripts are built.

### Meshes built in code

A mesh source adds procedural models to every scene, under names the scene
refers to, exactly like the built-in `builtin:cube`:

```cpp
class GameModels : public sfs::MeshSource
{
public:
  void build(sfs::MeshLibrary& library) override
  {
    library.add(towerModel(), "mygame:tower");
  }
};
```

The names appear in the Inspector's mesh picker. Prefix them with your game's
name to avoid clashes. If a scene names a mesh that doesn't exist yet (for
example, before the scripts are built), the entity keeps the name and draws
nothing; the Inspector shows it as "(missing)". The mesh appears once it
exists, and saving meanwhile doesn't lose it.
A scene that uses a component that isn't built yet keeps that component's data
and loads it once the module exists. See
[Scene files](../../runtime/serialization/index.md).

## Play, Stop and Build

| Command | Effect |
| --- | --- |
| **Play** (Ctrl+P) | Builds the scripts and reloads the module, then runs the scene: systems are added and game object scripts attached |
| **Stop** (Ctrl+P) | Ends Play and restores the scene exactly as it was before Play |
| **Scripts → Build** (Ctrl+B) | Builds and reloads the module without playing, so new scripts appear in the editor |

Build output streams into the console, and errors are highlighted. A failed
build leaves the editor in edit mode with the previous module still loaded.

Saving is disabled while playing, so a play session can't overwrite the
scene.

While playing, the game owns the camera and the keyboard **when the viewport
has focus**: click the viewport to control the game (its border turns green).
Clicking elsewhere, such as the Code panel, hands the keyboard back to the
editor, so typing never moves the game. The viewport's own camera controls and
shortcuts are off during Play.

## The game module

Each project has a `CMakeLists.txt` (created with the project) that builds
`scripts/` into a shared library:

```cmake
find_package(engine REQUIRED)
sfs_add_game_module(MyGame)
```

`sfs_add_game_module` runs `sfs-codegen` over the scripts and compiles the
generated registration in. The module exports a single entry point,
`sfsRegisterModule`. Because the project is plain CMake, you can open the folder
in any CMake-aware IDE to work on scripts outside the editor.

The editor builds the module in `.sfs/build/<platform>/`, which `.gitignore`
excludes. Each build links its compile database at `.sfs/compile_commands.json`,
where the project's `.clangd` points clangd. It uses the toolchain and engine it was built with, so the module
always matches the engine it runs in. A Windows editor cross-built from WSL runs
these builds back in WSL.

### How reloading works

A module can't be swapped while anything it created still exists: component
storage, game objects and systems all live in its code. So the editor:

1. snapshots the scene,
2. destroys it,
3. unregisters the old module's types and unloads it,
4. loads the new module, which registers its types,
5. creates a fresh scene and restores the snapshot into it.

The editor loads the module from a copy in the temp folder rather than from the
build output. A loaded DLL is locked on Windows, and loading the build output
directly would make the next build fail to overwrite it.
