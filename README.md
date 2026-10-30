# Segfault Simulator

A game engine with an ECS core, a Unity-style OOP layer for game code, and an
editor. It's one engine for flat 2D, isometric 2D and 3D games, drawn by one
renderer on every desktop platform and on the web. Build your game in C++
against the engine library (or in the editor), and optionally drive it with a
live Lua scripting API.

<p align="center">
  <a href="https://hurricane-pinecone.github.io/Segfault-Simulator" target="_blank" rel="noopener noreferrer">
    <strong>▶ Play Glowfall Isle (3D)</strong>
  </a>
  &nbsp;·&nbsp;
  <a href="https://hurricane-pinecone.github.io/Segfault-Simulator/samplePlatformer" target="_blank" rel="noopener noreferrer">
    <strong>▶ Play Neon Sprint (flat 2D)</strong>
  </a>
</p>

## Table of Contents

- [Overview](#overview)
- [Documentation](#documentation)
- [Prerequisites](#prerequisites)
- [Using SFS in Your Game](#using-sfs-in-your-game)
  - [Get the Engine Package](#get-the-engine-package)
  - [Require and Link It](#require-and-link-it)
  - [Raw CMake Install (no Conan)](#raw-cmake-install-no-conan)
  - [Runtime Assets](#runtime-assets)
- [The Samples](#the-samples)
- [Working on the Engine](#working-on-the-engine)

## Overview

SegFaultSimulator (SFS) is one engine where flat 2D, isometric 2D and 3D are
styles of the same world:

- An **ECS core** of entities, components and systems, with one component set
  for every style (transforms, cameras, lights, sprites, particle emitters,
  2D and 3D physics, meshes, grass), reflected so they are editable and
  saveable.
- **Three styles**, set per scene:
  - **Flat 2D**: sprites on a plane, seen straight on, for side-on or
    top-down games.
  - **Isometric 2D**: a tile grid with heights, drawn as a true orthographic
    isometric picture with real depth: sprites, voxel terrain, decals and
    particles placed on the grid.
  - **3D**: a free-camera world with models, material graphs, grass and Jolt
    physics.
- **One renderer** (`IRenderer`) and one render system for all three, with
  render modules for sprites, meshes, particles, decals, voxels, grass and UI,
  and the same lighting everywhere: sun and point-light shadows, clustered
  point lights, bloom, depth of field and haze.
- **WebGPU everywhere**: wgpu-native on Windows, Linux and macOS (Vulkan,
  Direct3D 12 or Metal, with an OpenGL fallback), and the browser's WebGPU on
  the web. Shaders are WGSL. Render backends are pluggable.
- **SfsEditor**, a docking editor for scenes of every style, with prefabs,
  material graphs, native C++ game scripts built on Play, and game export.
- A **live Lua scripting / modding API** and an in-game dev console.

It ships as three libraries:

| Target                 | Contents                                                     | Dependencies |
| ---------------------- | ------------------------------------------------------------ | ------------ |
| `sfs::engine`          | The runtime: game loop, scenes, rendering, input, assets, UI. | SDL2, Dear ImGui |
| `sfs::engine-webgpu`   | The WebGPU render backend and renderer.                      | wgpu-native |
| `sfs::engine-core`     | The ECS, scripting, particle and rendering-data core, no rendering. | none |

Link `sfs::engine-webgpu` (it brings `sfs::engine`) to build a game;
`sfs::engine-core` is for headless tools or projects that bring their own
rendering.

## Documentation

For in-depth documentation, see: **[hurricane-pinecone.github.io/Segfault-Simulator/docs](https://hurricane-pinecone.github.io/Segfault-Simulator/docs/)**.
A bot auto writes the docs, so if they're shit blame Bill Gates or whoever birthed Claude.

## Prerequisites

SFS is distributed with **Conan (v2)** and built with **CMake**. On macOS:

```bash
brew install conan cmake
conan profile detect --force
```

## Using SFS in Your Game

### Get the Engine Package

SFS is not published to a public Conan remote, so build the package from source
into your local Conan cache once:

```bash
git clone https://github.com/hurricane-pinecone/Segfault-Simulator.git
cd Segfault-Simulator
conan create .                          # the full runtime
```

To package only the dependency-free core (no SDL or GPU):

```bash
conan create . -o "&:core_only=True"    # sfs::engine-core
```

### Require and Link It

Add the engine to your game's `conanfile.txt`:

```ini
[requires]
sfs-engine/0.1.0

[generators]
CMakeDeps
CMakeToolchain
```

Then link it in your `CMakeLists.txt`:

```cmake
find_package(engine REQUIRED)
target_link_libraries(myGame PRIVATE sfs::engine-webgpu)   # or sfs::engine-core
```

The SDL / imgui / wgpu-native closure comes in transitively, so you do not
list those yourself.

### Raw CMake Install (no Conan)

The engine also installs as a plain CMake package:

```bash
cmake --install <engine-build-dir> --prefix /path/to/engine
```

Point your build at it and consume the same targets:

```cmake
# cmake -D CMAKE_PREFIX_PATH=/path/to/engine ...
find_package(engine REQUIRED)
target_link_libraries(myGame PRIVATE sfs::engine-webgpu)
```

### Runtime Assets

The engine loads a few runtime assets of its own (e.g. the default font).
`find_package(engine)` sets `ENGINE_ASSET_DIR` to their location; copy them next
to your executable:

```cmake
add_custom_command(TARGET myGame POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${ENGINE_ASSET_DIR}" "$<TARGET_FILE_DIR:myGame>/assets")
```

## The Samples

Two games made in SfsEditor, both playable in the browser (links above):

- `samples/showcase3d/` is **Glowfall Isle**, the 3D sample: an island with
  models, material graphs, grass, physics and a day that passes.
- `samples/platformer2d/` is **Neon Sprint**, the flat 2D sample: a neon
  runner with sprites, point lights and particles.

Each is an editor project (`project.sfsproj`): open its folder in SfsEditor
to play it, change it, or export it as a game. The web build publishes them as
the site's root and `samplePlatformer/` page.

For building the engine itself, the bundled workspace build, and tooling, see
[engine development](./engine/README.md).

## Working on the Engine

CI checks that the engine's C++ is formatted with clang-format 17.0.6 (the
repo's `.clang-format`). A pre-commit hook formats what you commit, so you don't
have to think about it. Turn it on once per clone:

```bash
git config core.hooksPath .githooks
```

It formats only the files you're committing, in the folders CI checks, and only
what's staged. The first time, it installs clang-format 17.0.6 into `.git/` if
that version isn't on your PATH (other versions format slightly differently).
Skip it for one commit with `git commit --no-verify`.
