# Segfault Simulator

This is a lightweight engine built using the ECS pattern, with a Unity style OOP layer on top for the game client.

<p align="center">
  <a href="https://hurricane-pinecone.github.io/Segfault-Simulator" target="_blank" rel="noopener noreferrer">
    <strong>▶ Play the dogshit sample game</strong>
  </a>
</p>

![ECS Diagram](./docs/images/ecs.png)

## Table of Contents

- [Overview](#overview)
- [Initial Setup](#initial-setup)
- [Debug Build](#debug-build)
- [Run](#run)
- [Release Build](#release-build)
  - [Web build](#web-build)
- [LSP / clangd setup](#lsp--clangd-setup)
- [Rebuilding](#rebuilding)
  - [TL;DR](#tldr)
  - [Normal rebuild](#normal-rebuild)
  - [If `conanfile.txt` changes](#if-conanfiletxt-changes)
  - [If `CMakeLists.txt` changes](#if-cmakeliststxt-changes)
  - [If compiler/toolchain changes](#if-compilertoolchain-changes)
- [Clean Build](#clean-build)
- [Assets](#assets)
- [Important](#important)
- [Optional Aliases (zsh)](#optional-aliases-zsh)
- [Tooling](#tooling)
  - [Leak Detection](#leak-detection)
  - [Tracy Profiling](#tracy-profiling)
    - [Install Tracy Profiler (macOS)](#install-tracy-profiler-macos)
    - [Build and Run in Profiling Mode](#build-and-run-in-profiling-mode)
    - [Tracy Configuration](#tracy-configuration)

## Overview

This project uses:

- **Conan (v2)** → dependency management
- **CMake + Presets** → build system

The project is structured as:

```text
engine/ → library
sampleGame/   → executable
```

- Engine is a reusable **library**
- sampleGame is the **entry point**
- Assets live next to the executable at runtime

## Initial Setup

### 1. Install dependencies (macOS)

```bash
brew install conan cmake
```

Initialize Conan:

```bash
conan profile detect --force
```

## Debug Build

### 2. Install dependencies

```bash
conan install . --build=missing -s build_type=Debug
```

### 3. Configure

```bash
cmake --preset debug
```

### 4. Build

```bash
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

### Web build

The web build can't be run in debug because ImGUI is stripped from the build

```bash
rm -rf build-web
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release
cmake --build build-web
cd build-web/bin
python3 server.py
```

## LSP / clangd setup

```bash
ln -sf build/Debug/compile_commands.json compile_commands.json
```

Restart your editor after this.

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

### If `conanfile.txt` changes

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

## Assets

Assets are automatically copied to the executable directory:

```text
build/Debug/bin/
  sampleGame
  assets/
```

Game code uses:

```cpp
const std::string ASSET_ROOT = "./assets/";
```

## Important

Always run the game using one of these:

```bash
cmake --build --preset debug --target run
```

or:

```bash
cd build/Debug/bin
./sampleGame
```

Do **not** run from repo root:

```bash
./build/Debug/bin/sampleGame
```

This will break asset paths.

## Optional Aliases (zsh)

```bash
echo "alias crun='cmake --build --preset debug --target run'" >> ~/.zshrc
echo "alias crun-release='cmake --build --preset release --target run'" >> ~/.zshrc
source ~/.zshrc
```

Run with:

```bash
crun
crun-release
```

## Tooling

### Leak Detection

```bash
./scripts/run_leaks.sh
```

If it needs permissions

```bash
chmod +x scripts/run_leaks.sh
```

### Tracy Profiling

![Tracy](./docs/images/tracy.png)

#### Install Tracy Profiler (macOS)

```bash
brew install tracy
```

Launch the profiler UI:

```bash
tracy-profiler
```

---

#### Build and Run in Profiling Mode

Add this alias to your shell config (`~/.zshrc`, `~/.bashrc`, etc):

```bash
alias crun-profile='conan install . --build=missing -s build_type=RelWithDebInfo && cmake --preset conan-relwithdebinfo && cmake --build --preset conan-relwithdebinfo --target run'
```

Reload your shell:

```bash
source ~/.zshrc
```

Then build and run the engine with Tracy enabled:

```bash
crun-profile
```

---

#### Tracy Configuration

Tracy is only enabled in `RelWithDebInfo` builds.

Use the shared profiling wrapper so Tracy is stripped from other build types automatically.

```cpp
#include "engine/utils/profiling.h"
```
