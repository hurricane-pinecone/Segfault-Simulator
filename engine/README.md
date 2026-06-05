# Engine Development

Building and working on the SegFaultSimulator engine itself. To consume the
engine in your own game, see the [root README](../README.md).

## Table of Contents

- [Repo Layout](#repo-layout)
- [Prerequisites](#prerequisites)
- [Debug Build](#debug-build)
- [Run](#run)
- [Release Build](#release-build)
- [Web Build](#web-build)
- [Testing](#testing)
- [Rebuilding](#rebuilding)
- [Clean Build](#clean-build)
- [LSP / clangd Setup](#lsp--clangd-setup)
- [Assets](#assets)
- [Running the Game](#running-the-game)
- [Optional Aliases (zsh)](#optional-aliases-zsh)
- [Tooling](#tooling)
  - [Leak Detection](#leak-detection)
  - [Tracy Profiling](#tracy-profiling)

The build uses **Conan (v2)** for dependencies and **CMake + presets** for the
build itself.

## Repo Layout

```text
engine/      → the engine library (engine-core + render runtime)
sampleGame/  → a game built on the engine
```

The workspace build below compiles the engine and the sample together in one
tree, which is the fast path for iterating on engine code.

## Prerequisites

macOS:

```bash
brew install conan cmake
conan profile detect --force
```

## Debug Build

```bash
conan install . --build=missing -s build_type=Debug
cmake --preset debug
cmake --build --preset debug
```

## Run

```bash
cmake --build --preset debug --target run
```

## Release Build

```bash
conan install . --build=missing -s build_type=Release
cmake --preset release
cmake --build --preset release
cmake --build --preset release --target run
```

## Web Build

ImGui is stripped from the web build, so it has no debug variant.

```bash
rm -rf build-web
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release
cmake --build build-web
cd build-web/bin
python3 server.py
```

Or, with the [`crun-web`](#optional-aliases-zsh) alias (configures, builds, and
serves in one step):

```bash
crun-web
```

## Testing

Tests link `engine-core`, which has no third-party dependencies (vendored Lua +
glm). With `ENGINE_CORE_ONLY` the suite builds from just CMake + a compiler — no
Conan, SDL, or OpenGL — so it is fast, and is what CI runs:

```bash
cmake -S . -B build-core -DENGINE_CORE_ONLY=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-core
ctest --test-dir build-core --output-on-failure
```

Or, with the [`crun-tests`](#optional-aliases-zsh) alias (configure, build, and
run in one step):

```bash
crun-tests
```

The tests also build as part of a full `cmake --preset debug` build and run with
`ctest --test-dir build/Debug`.

## Rebuilding

### TL;DR

```bash
conan install . --build=missing -s build_type=Debug
cmake --preset debug
cmake --build --preset debug --target run
```

### Normal rebuild

```bash
cmake --build --preset debug
```

### If `conanfile.py` changes

```bash
conan install . --build=missing -s build_type=Debug
cmake --preset debug
```

### If `CMakeLists.txt` changes

```bash
cmake --preset debug
```

### If compiler/toolchain changes

```bash
conan profile detect --force
```

## Clean Build

```bash
rm -rf build
rm -rf engine/build

conan install . --build=missing -s build_type=Debug
cmake --preset debug
cmake --build --preset debug
```

## LSP / clangd Setup

```bash
ln -sf build/Debug/compile_commands.json compile_commands.json
```

Restart your editor after this.

## Assets

The build copies assets next to the executable:

```text
build/Debug/bin/
  sampleGame
  assets/
```

Game code loads them relative to the working directory:

```cpp
const std::string ASSET_ROOT = "./assets/";
```

## Running the Game

Run from the executable's directory so the relative asset paths resolve:

```bash
cmake --build --preset debug --target run
```

or:

```bash
cd build/Debug/bin
./sampleGame
```

Running from the repo root breaks asset paths:

```bash
./build/Debug/bin/sampleGame   # don't
```

## Optional Aliases (zsh)

Add these to your shell config (`~/.zshrc`):

```bash
alias crun='conan install . --build=missing -s build_type=Debug && cmake --preset debug && cmake --build --preset debug --target run'
alias crun-release='conan install . --build=missing -s build_type=Release && cmake --preset release && cmake --build --preset release --target run'
alias crun-profile='conan install . --build=missing -s build_type=RelWithDebInfo && cmake --preset conan-relwithdebinfo && cmake --build --preset conan-relwithdebinfo --target run'
alias crun-tests='cmake -S . -B build-core -DENGINE_CORE_ONLY=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-core && ctest --test-dir build-core --output-on-failure'
alias crun-web='emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web --target run'
alias crun-sample-pkg='conan create . -s build_type=Release --build=missing && (cd sampleGame && conan install . --build=missing -s build_type=Release && cmake --preset conan-release && cmake --build --preset conan-release --target run)'
```

Reload your shell:

```bash
source ~/.zshrc
```

Then, from the project root:

```bash
crun            # debug build + run
crun-release    # release build + run
crun-profile    # RelWithDebInfo build + run (Tracy enabled)
crun-tests      # build + run tests (CTest)
crun-web        # wasm build + serve (requires emsdk on PATH)
crun-sample-pkg # release: publish the engine package, build + run the sample against it
```

## Tooling

### Leak Detection

```bash
./scripts/run_leaks.sh
```

If the script needs execute permission:

```bash
chmod +x scripts/run_leaks.sh
```

### Tracy Profiling

![Tracy](../docs/images/tracy.png)

Tracy is enabled only in `RelWithDebInfo` builds. Install the profiler UI
(macOS):

```bash
brew install tracy
tracy-profiler
```

Build and run with Tracy enabled via the [`crun-profile`](#optional-aliases-zsh)
alias:

```bash
crun-profile
```

Include the shared profiling wrapper so Tracy is compiled out of other build
types automatically:

```cpp
#include "engine/core/util/profiling.h"
```
