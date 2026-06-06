# SegFaultSimulator

SegFaultSimulator (**sfs**) is a lightweight 2D game engine with an **ECS core**
and a Unity-style OOP layer for game code. It renders flat 2D or isometric
heightfield scenes through one shared core, and can be driven live with a Lua
scripting API. You build your game in **C++** against the engine library; native
(macOS / Linux) and web (Emscripten / WebGL2) targets are supported.

It ships as two libraries:

| Target | Contents | Dependencies |
| --- | --- | --- |
| `sfs::engine` | the full render runtime | SDL2, OpenGL |
| `sfs::engine-core` | the ECS, scripting, and particle core — no rendering | none |

Link `sfs::engine` to build a game; `sfs::engine-core` is for headless tools or
projects that bring their own rendering.

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
conan create .                       # sfs::engine (full runtime)
conan create . -o "&:core_only=True" # OR just sfs::engine-core (no SDL/OpenGL)
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
target_link_libraries(myGame PRIVATE sfs::engine)   # or sfs::engine-core
```

The SDL / imgui / GLEW closure comes in transitively — you don't list those
yourself. (Prefer plain CMake? The engine also installs as a CMake package:
`cmake --install <build-dir> --prefix /path/to/engine`, then point
`CMAKE_PREFIX_PATH` at it and consume the same targets.)

**Runtime assets.** The engine loads a few assets of its own (e.g. the default
font). `find_package(engine)` sets `ENGINE_ASSET_DIR`; copy them next to your
executable:

```cmake
add_custom_command(TARGET myGame POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${ENGINE_ASSET_DIR}" "$<TARGET_FILE_DIR:myGame>/assets")
```

From here your game is a `Game` subclass that creates a `Scene`, which holds your
entities, systems, and game objects.

## Suggested reading

New to the engine? Start with the fundamentals, in order:

1. **[Entities, components & systems](./core/ecs/index.md)** — the ECS data model
   and the OOP layer over it: spawning entities, attaching components, and writing
   systems.
2. **[Game & scenes](./runtime/game-and-scenes/index.md)** — the `Game → Scene → System`
   structure and the per-frame hooks where your code runs.

Then explore by layer:

- **[Engine core](./core/index.md)** — the dependency-free half: ECS, the particle
  engine, scripting, and the engine-vs-game ownership model.
- **[Runtime](./runtime/index.md)** — the SDL/OpenGL half: the frame loop,
  rendering, and using those systems in a game.

## Conventions

- Snippets are engine-only and game-agnostic — substitute your own types wherever
  a placeholder appears (`MyGame`, `MyScene`, a component of your own, …).
- Coordinate units differ by render path: world **tiles** on the isometric path,
  **pixels** on the flat path.
