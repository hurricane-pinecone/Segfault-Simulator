# Engine Development

Building and working on the SegFaultSimulator engine itself. To consume the
engine in your own game, see the [root README](../README.md).

## Table of Contents

- [Repo Layout](#repo-layout)
- [Documentation](#documentation)
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
  - [Code Formatting](#code-formatting)
  - [Cross-Compiler Lint (GCC)](#cross-compiler-lint-gcc)
  - [Leak Detection](#leak-detection)
  - [Tracy Profiling](#tracy-profiling)

The build uses **Conan (v2)** for dependencies and **CMake + presets** for the
build itself.

## Repo Layout

```text
engine/       → the engine library (engine-core + render runtime)
engine/docs/  → contributor docs (internal engine design)
sampleGame/   → a game built on the engine
```

The workspace build below compiles the engine and the sample together in one
tree, which is the fast path for iterating on engine code.

## Documentation

There are two separate documentation sets, by audience:

- **Contributor docs** live in `engine/docs/`. They cover how the engine is built internally
  and why, for people working **on** the engine. Plain Markdown, read on GitHub,
  not published anywhere.
- **User docs** live in the repo-root [`docs/`](../docs/) tree and are published
  as the MkDocs site. They are game-developer guides for people building games
  **with** the engine (and are auto-drafted by the docs workflow).

Rule of thumb: how a subsystem works internally goes in `engine/docs/`; how a
game developer uses a feature goes in `docs/`.

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

The web build emits both samples into the Pages artifact (`build-web/bin`): the
isometric sample at the root (`index.html`) and the platformer under
`samplePlatformer/`. The `deploy-web` workflow publishes that directory, so they
land at the site root and `<site>/samplePlatformer` respectively. Serve the
platformer locally with `python3 build-web/bin/samplePlatformer/server.py`.

## Testing

The engine ships its own tests, built on a small hand-rolled harness in
`engine/tests/testHarness.h`. It uses no third-party test framework, which keeps the
core dependency-free. A test is a standalone executable: group checks with `TEST("...")`,
assert with `CHECK(...)`, and end with `return testing::report("name")`. `CHECK`
decomposes the expression, so a failing `CHECK(a == b)` prints the actual operand
values; `testing::approx(a, b)` compares floats with a tolerance.

The tests split to match the two libraries:

- **Core tests** (`engine/tests/core/`) link `engine-core` only, with no SDL, GL, or
  Conan. Register one with `add_core_test(name path/to/test.cpp)` in
  `engine/tests/core/CMakeLists.txt`.
- **Runtime tests** (`engine/tests/runtime/`) link the full `engine` and drive
  systems and render modules through their seams (a mock `IQuadRenderer`, stub
  services), so they run headless: no window, no GPU, no display. Register one with
  `add_runtime_test(name path/to/test.cpp)` in `engine/tests/runtime/CMakeLists.txt`.

Test files mirror the source tree they cover. A module test for
`src/runtime/rendering/modules/` lives in `engine/tests/runtime/rendering/modules/`.
Each test carries a CTest label (`core` or `runtime`) so you can run one group on
its own. The line for what is tested is drawn at rendered pixels: everything up to
the GL draw call (command building, batching, depth assignment) is covered by
inspecting what a system submits to the mock renderer, but framebuffer contents are
not asserted.

### Build and run

With `ENGINE_CORE_ONLY` the core suite builds from just CMake and a compiler, with no
Conan, SDL, or OpenGL, so it is fast, and is what CI's core job runs:

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

The runtime tests build as part of a full `cmake --preset debug` build. Filter by
label with `ctest`:

```bash
ctest --test-dir build/Debug --output-on-failure   # core + runtime
ctest --test-dir build/Debug -L core               # core only
ctest --test-dir build/Debug -L runtime            # runtime only
```

### Continuous integration

These blocking jobs run on every push and pull request:

- **test-core**: core-only configure, runs the `core` label under GCC. Needs no
  dependencies.
- **sanitize-core**: the same core-only build under AddressSanitizer and
  UndefinedBehaviorSanitizer. With test-core, this is the gate a standalone core
  release relies on, so it stays free of the runtime stack.
- **test-runtime**: full native build, runs the `runtime` label.
- **sanitize-runtime**: the full build with `ENGINE_SANITIZE=ON` (the flags apply
  to the engine targets only, so the prebuilt dependencies stay uninstrumented),
  running the runtime tests under the sanitizers.
- **format**: enforces `.clang-format` (see [Code Formatting](#code-formatting)).
- **build**: the web build, gated on the four test jobs.

The web deploy waits on all of them. The core jobs build under GCC, which is
stricter about includes than the macOS toolchain. See
[Cross-Compiler Lint (GCC)](#cross-compiler-lint-gcc) to catch that locally.

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

### Code Formatting

C and C++ sources are formatted with clang-format against the repository's
`.clang-format` (an LLVM base with Allman braces, an 80 column limit, and one
parameter per line). The vendored `engine/lib` tree keeps its own style and is left
alone.

CI pins **clang-format 17.0.6** and fails a pull request on any unformatted file.
The version is pinned because output differs across clang-format releases, and a
system clang-format (such as the one Apple ships) is a slightly different fork.
Install the same version so your formatting matches CI:

```bash
python3 -m venv ~/.local/share/sfs-clang-format
~/.local/share/sfs-clang-format/bin/pip install clang-format==17.0.6
ln -sf ~/.local/share/sfs-clang-format/bin/clang-format ~/.local/bin/clang-format
```

A pre-commit hook formats staged sources for you. Enable it once per clone:

```bash
git config core.hooksPath .githooks
```

`git commit` then reformats the staged C and C++ files and restages them, so
nothing unformatted reaches a commit. To format the whole tree by hand:

```bash
find engine/src engine/include engine/tests sampleGame/src samplePlatformer/src \
  \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o -name '*.inl' \) \
  ! -path '*/engine/lib/*' -print0 | xargs -0 clang-format -i
```

### Cross-Compiler Lint (GCC)

CI compiles on **Linux (GCC + libstdc++)**. The macOS toolchain (**Clang +
libc++**) pulls many standard headers in transitively while libstdc++ does not, so
a file that uses a `std::` symbol without including its header builds fine locally
but fails CI with `X is not a member of std`. `scripts/gcc-lint.sh` catches this
before you push: it runs a GCC syntax-only pass (parse only, no link) over the core
and runtime sources and tests inside a cached Docker image, reproducing the CI
compiler so you do not discover a missing include through repeated push-and-wait
cycles.

With the Docker daemon running, from the project root:

```bash
./scripts/gcc-lint.sh                       # lint every core and runtime TU + tests
./scripts/gcc-lint.sh engine/src/foo.cpp    # lint only the given file(s)
```

It exits non-zero on a failure and prints the offending file with GCC's own
diagnostic, which names the exact header to add, for example `<limits>` for
`std::numeric_limits`, `<algorithm>` for `std::sort`, `<utility>` for `std::move`,
`<cstring>` for `std::strrchr`. The first run builds the image (gcc:14 plus the SDL
and GLEW headers, from `scripts/gcc-lint.Dockerfile`); later runs finish in a few
seconds. Vendored code under `engine/lib` is not linted, since it is not ours to
change.

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
