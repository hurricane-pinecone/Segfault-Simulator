# Segfault Simulator

Segfault Simulator (SFS) is a general-purpose C++ game engine with an editor. One ECS and one renderer make flat 2D,
isometric 2D and 3D games, which run on the desktop and in the browser.

<p align="center">
  <a href="https://hurricane-pinecone.github.io/Segfault-Simulator" target="_blank" rel="noopener noreferrer"><strong>▶ Play the 3D sample</strong></a>
  &nbsp;·&nbsp;
  <a href="https://hurricane-pinecone.github.io/Segfault-Simulator/samplePlatformer" target="_blank" rel="noopener noreferrer"><strong>▶ Play the 2D sample</strong></a>
  &nbsp;·&nbsp;
  <a href="https://hurricane-pinecone.github.io/Segfault-Simulator/docs/"><strong>Documentation</strong></a>
</p>

## Features

**Engine core**
- An ECS of entities, components and systems, with cached queries, observers and scene singletons.
- Reflection generated from your headers by `sfs-codegen`. Fields take `[[sfs::...]]` attributes, and there are no
  registration macros.
- Typed events between systems, parent and child hierarchies, prefabs, and scriptable objects (`.asset` files) for
  data many entities share.
- A live Lua modding API and an in-game dev console.

**Runtime**
- One WebGPU renderer for every style: wgpu-native on the desktop (Vulkan, Direct3D 12 or Metal, with an OpenGL
  fallback) and the browser's WebGPU on the web. Material graphs, shadows, clustered point lights, bloom and depth of
  field.
- 3D physics on Jolt (rigid bodies, colliders, raycasts), and built-in 2D physics with swept collision and triggers.
- Navigation for AI vehicles: path planning round colliders, and traffic between agents.
- Declarative game UI, particles and decals, skeletal animation, and voxel terrain.

**Tools**
- **SfsEditor**, a docking editor for scenes of every style. It has prefabs, material graphs and an embedded Neovim
  code panel. It builds and reloads your C++ game scripts when you press Play, and exports a desktop game or a web
  page.
- **sfs-lsp**, a language server that runs beside clangd. It completes `[[sfs::...]]` attributes, shows codegen's
  diagnostics as you type, and lints use of the engine's API and architecture rules (`sfs-lsp --fix` applies fixes).

**Platforms**
- Windows and Linux, both built and tested in CI, and the web through Emscripten.
- macOS is untested. The renderer and the Conan recipe support it, but no CI job builds it.

## Status

SFS is early and in active development. The API still changes between versions, and there are no releases yet, so
you build it from source.

## Documentation

The guide to making a game with SFS is at
**[hurricane-pinecone.github.io/Segfault-Simulator/docs](https://hurricane-pinecone.github.io/Segfault-Simulator/docs/)**.
Good places to start:

- [The editor](https://hurricane-pinecone.github.io/Segfault-Simulator/docs/editor/): projects, scenes, panels,
  prefabs and exporting.
- [Entities, components and systems](https://hurricane-pinecone.github.io/Segfault-Simulator/docs/core/ecs/): the data
  model your game is built on.
- [Game scripts](https://hurricane-pinecone.github.io/Segfault-Simulator/docs/editor/scripting/): writing gameplay in
  C++ in the editor.
- [Lua scripting](https://hurricane-pinecone.github.io/Segfault-Simulator/docs/core/scripting/): a live modding API
  for your game.
- [Game and scenes](https://hurricane-pinecone.github.io/Segfault-Simulator/docs/runtime/game-and-scenes/): the frame
  loop, if you write a game against the engine library without the editor.

## Getting started

You need CMake 3.24 or newer, Conan 2 (`pip install "conan==2.*"`), a C++23 compiler, and a 64-bit CPU with AVX2.
The editor's code panel uses Neovim 0.11 or newer if you have it. Build the editor and run it from its own folder,
so it finds its assets:

```bash
git clone https://github.com/hurricane-pinecone/Segfault-Simulator.git
cd Segfault-Simulator
conan profile detect --force
conan install . --build=missing -s build_type=Debug
cmake --preset debug
cmake --build --preset debug
cd build/Debug/bin && ./SfsEditor
```

On Linux, Conan builds SDL from source and may need system packages for it. Add
`-c tools.system.package_manager:mode=install -c tools.system.package_manager:sudo=True` to `conan install` to let it
install them.

In the editor, **File → New Project** creates a project folder (`project.sfsproj`, `scenes/`, `assets/`, `scripts/`).
Add C++ scripts under `scripts/`, press **Play** to build them and run the scene, and use **File → Export Game** to
write a playable copy. The [editor guide](https://hurricane-pinecone.github.io/Segfault-Simulator/docs/editor/) covers
the rest.

On Windows, the editor needs a shared engine to load your game module, and MSVC builds the engine statically. Build
the Windows editor with MinGW instead (see below).

## Building from source

The build uses Conan 2 for dependencies and CMake presets for the build. Engine internals, profiling, lint tooling
and shell aliases are in [engine/README.md](engine/README.md).

### Presets

`CMakePresets.json` has `debug`, `release` and `relwithdebinfo` (with Tracy profiling). Each builds into
`build/<BuildType>/` after a `conan install` of the same build type:

```bash
conan install . --build=missing -s build_type=Release
cmake --preset release
cmake --build --preset release
```

Pass options at configure time, such as `cmake --preset debug -DENGINE_BUILD_SAMPLE=OFF`. The defaults below are for
a top-level build:

| Option | Default | Effect |
| --- | --- | --- |
| `ENGINE_CORE_ONLY` | `OFF` | Build only `engine-core` and its tests, with no Conan or third-party libraries. |
| `ENGINE_BUILD_EDITOR` | `ON` | Build SfsEditor, and SfsPlayer, which exported games run on. |
| `ENGINE_BUILD_TESTS` | `ON` | Build the engine test suite. |
| `ENGINE_BUILD_SAMPLE` | `ON` | Build the sample games in a web build. |
| `ENGINE_SHARED` | `ON` (`OFF` with MSVC) | Build the engine as shared libraries. The editor needs this. |
| `ENGINE_SANITIZE` | `OFF` | Build with AddressSanitizer and UndefinedBehaviorSanitizer. |
| `SFS_CODEGEN_EXECUTABLE` | empty | A host `sfs-codegen`, needed when cross-compiling or building for the web. |

### Using the engine in your own CMake project

SFS isn't on a public Conan remote, so package it into your local Conan cache:

```bash
conan create .                         # the full runtime
conan create . -o "&:core_only=True"   # engine-core only, with no SDL or GPU
```

Then require `sfs-engine/0.1.0` in your game's `conanfile.txt` (with the `CMakeDeps` and `CMakeToolchain`
generators) and link one of its targets:

```cmake
find_package(engine REQUIRED)
target_link_libraries(myGame PRIVATE sfs::engine-webgpu)   # or sfs::engine-core
```

| Target | Contents |
| --- | --- |
| `sfs::engine-webgpu` | The WebGPU renderer. It brings `sfs::engine`, so link this to build a game. |
| `sfs::engine` | The runtime: game loop, scenes, input, assets, UI and physics, on SDL2 and Dear ImGui. |
| `sfs::engine-core` | The ECS, scripting, particles and rendering data, with no dependencies. For headless tools. |

Without Conan, install a workspace build as a plain CMake package and point `CMAKE_PREFIX_PATH` at it:

```bash
cmake --install build/Release --prefix /path/to/engine
```

The package's `bin/` holds the host tools `sfs-codegen` and `sfs-lsp`. On Windows, call `sfs_deploy_runtime(myGame)`
after `find_package(engine)` to copy the engine's DLLs next to your executable. Either way, `find_package(engine)`
sets `ENGINE_ASSET_DIR` to the engine's runtime assets (such as the default font), which your game copies next to
its executable.

### Windows (MinGW cross-build)

The Windows editor is cross-built from Linux or WSL with MinGW-w64 (`g++-mingw-w64-x86-64-posix` on Debian and
Ubuntu). It needs a native build first for its host `sfs-codegen`, and a Conan host profile named `windows-mingw`:
`os=Windows`, `arch=x86_64`, `compiler=gcc` 13 with `compiler.threads=posix` and `compiler.exception=seh`, the
`x86_64-w64-mingw32-*-posix` tools in `tools.build:compiler_executables`, and `-static-libgcc -static-libstdc++` in
the link flags.

```bash
conan install . -of build-win -pr:h windows-mingw -pr:b default --build=missing
cmake -S . -B build-win/build/Release -DCMAKE_BUILD_TYPE=Release \
  --toolchain "$PWD/build-win/build/Release/generators/conan_toolchain.cmake" \
  -DSFS_CODEGEN_EXECUTABLE="$PWD/build/Debug/bin/sfs-codegen"
cmake --build build-win/build/Release
```

The editor and its DLLs land in `build-win/build/Release/bin/`. A native MSVC build works for the engine and its
tests, as in CI's Windows job: `conan install . --build=missing -s build_type=Release -s compiler.cppstd=23
-o "*:shared=False"`, then `cmake --preset conan-default` and `cmake --build --preset conan-release`.

### Web

The web build draws on the browser's WebGPU, so it runs in a recent Chrome, Edge or Safari, or Firefox 141 or newer
on Windows. It needs Emscripten on your `PATH` (CI pins 6.0.13, because 7.0.0 can't compile Jolt) and a native build
for the host `sfs-codegen`. It builds the two sample games into `build-web/bin/`:

```bash
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release \
  -DSFS_CODEGEN_EXECUTABLE="$PWD/build/Debug/bin/sfs-codegen"
cmake --build build-web
cd build-web/bin && python3 -m http.server
```

To export your own project as a web page, use the editor's export, which also needs Emscripten.

### Tests

A preset build includes the tests. Each has a label (`core`, `runtime` or `webgpu`), so you can run one group. The
core tests also build on their own with only CMake and a compiler:

```bash
ctest --test-dir build/Debug --output-on-failure -L runtime

cmake -S . -B build-core -DENGINE_CORE_ONLY=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-core
ctest --test-dir build-core --output-on-failure
```

### Formatting

CI checks the engine's C++ with clang-format 17.0.6 against the repo's `.clang-format`. Turn on the pre-commit hook
once per clone, and it formats the files you commit:

```bash
git config core.hooksPath .githooks
```

It installs clang-format 17.0.6 into `.git/` if that version isn't on your `PATH`. Skip it for one commit with
`git commit --no-verify`.

## Licence

SFS is licensed under the [PolyForm Shield License 1.0.0](LICENSE.md). Anyone may use it, and make and sell games
with it, royalty-free. What you may not do is offer a product that competes with SFS. The games you make with SFS
are yours.

Copyright 2026 Jimmy Couch. Third-party libraries in `engine/lib` and those fetched at build time keep their own
licences.

## Contributing

Outside contributors sign the [Contributor License Agreement](CLA.md) once, before their first pull request is merged.
You keep the copyright in your contribution and give the maintainer a licence to use it. A bot comments on your first
pull request with the sentence to post to sign it.
