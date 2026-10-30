# SegFaultSimulator

SegFaultSimulator (**sfs**) is a game engine with an **ECS core**, a Unity-style
OOP layer for game code, and an editor. It's one engine for three styles of
game, which are styles of the same world rather than separate engines:

- **Flat 2D**: sprites on a plane, seen straight on (platformers, top-down games).
- **Isometric 2D**: a tile grid with heights, drawn as a true orthographic
  isometric picture with real depth.
- **3D**: a free-camera world in metres, with models, materials, grass and
  physics.

All three use the same components, the same render system and **one
renderer**, with the same lights, shadows, bloom, particles and UI. The renderer
runs on WebGPU: natively through wgpu-native (Vulkan, Direct3D 12 or Metal, with
an OpenGL fallback) on Windows, Linux and macOS, and in the browser on the
browser's WebGPU. Render backends are pluggable, and the editor edits games of
every style. You build your game in **C++** against the engine library, and can
drive it live with a Lua scripting API.

It ships as three libraries:

| Target | Contents | Dependencies |
| --- | --- | --- |
| `sfs::engine` | the runtime: game loop, scenes, rendering, input, assets, UI, physics | SDL2, Dear ImGui |
| `sfs::engine-webgpu` | the WebGPU render backend and renderer | wgpu-native (the browser's WebGPU on the web) |
| `sfs::engine-core` | the ECS, scripting, particle and rendering-data core, no rendering | none |

Link `sfs::engine-webgpu` (which brings `sfs::engine` with it) to build a
game; `sfs::engine-core` is for headless tools or projects that bring their
own rendering.

## Getting started

SFS is distributed with **Conan (v2)** and built with **CMake**.

**Prerequisites** (macOS shown; use your platform's packages otherwise):

```bash
brew install conan cmake
conan profile detect --force
```

**Build the engine package.** SFS isn't on a public Conan remote yet, so build it
into your local cache once:

```bash
git clone https://github.com/hurricane-pinecone/Segfault-Simulator.git
cd Segfault-Simulator
conan create .                       # the full runtime
conan create . -o "&:core_only=True" # OR just sfs::engine-core (no SDL or GPU)
```

**Require and link it.** In your game's `conanfile.txt`:

```ini
[requires]
sfs-engine/0.1.0

[generators]
CMakeDeps
CMakeToolchain
```

and your `CMakeLists.txt`:

```cmake
find_package(engine REQUIRED)
target_link_libraries(myGame PRIVATE sfs::engine-webgpu)   # or sfs::engine-core
```

The SDL / imgui / wgpu-native closure comes in transitively — you don't list
those yourself. (Prefer plain CMake? The engine also installs as a CMake
package: `cmake --install <build-dir> --prefix /path/to/engine`, then point
`CMAKE_PREFIX_PATH` at it and consume the same targets.)

**Runtime assets.** The engine loads a few assets of its own (e.g. the default
font). `find_package(engine)` sets `ENGINE_ASSET_DIR`; copy them next to your
executable:

```cmake
add_custom_command(TARGET myGame POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${ENGINE_ASSET_DIR}" "$<TARGET_FILE_DIR:myGame>/assets")
```

**Runtime libraries.** The engine libraries build as shared libraries by
default, so on Windows its DLLs (and wgpu-native's) need to sit next to your
executable too. Building with MSVC is the exception: an MSVC DLL only exports
symbols marked for export, and the engine marks none, so MSVC builds the
engine statically by default instead. After `find_package(engine)`, call the
`sfs_deploy_runtime` function it defines:

```cmake
sfs_deploy_runtime(myGame)
```

On Linux this is a no-op; the rpath CMake records already finds them.

From here your game is a `Game` subclass that creates a `Scene`, which holds your
entities, systems, and game objects, and a `RenderSystem` that draws them in the
style you choose. Or make the game in the [editor](./editor/index.md).

## Suggested reading

New to the engine? Start with the fundamentals, in order:

1. **[Entities, components & systems](./core/ecs/index.md)**: the ECS data model
   and the OOP layer over it: spawning entities, attaching components, and writing
   systems.
2. **[Game & scenes](./runtime/game-and-scenes/index.md)**: the `Game → Scene → System`
   structure and the per-frame hooks where your code runs.
3. **[Styles](./runtime/rendering/styles/index.md)**: setting up a flat,
   isometric or 3D game.

Then explore by layer:

- **[Engine core](./core/index.md)**: the dependency-free half: ECS, assets,
  rendering data, the particle engine, scripting, and the engine-vs-game
  ownership model.
- **[Runtime](./runtime/index.md)**: the frame loop, rendering, physics, UI,
  and using those systems in a game.
- **[Editor](./editor/index.md)**: SfsEditor, for making scenes and games of
  every style.

## Conventions

- Snippets are engine-only and game-agnostic — substitute your own types wherever
  a placeholder appears (`MyGame`, `MyScene`, a component of your own, …).
- Units depend on the style: a 3D world is in metres, Z up; a 2D game's
  positions are in its own units on its plane (a flat game's are often pixels,
  an isometric game's are tiles), with a height in `position.z`.
