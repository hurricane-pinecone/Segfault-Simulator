# Game Engine

## Overview

This project uses **Conan (C++ package manager)** and **CMake** for building and dependency management.

---

## Initial Setup

Here's some instructions for Mac. If you're on anything else, you're on your own. Good luck.  
I also don't use Visual Studio so have no idea how to generate a VS proj so good luck with that.

### 1. Install dependencies

```bash
brew install conan cmake
conan profile detect --force
conan install . --build=missing -s build_type=Debug
```

### 2. Configure project

```bash
cmake --preset conan-debug
```

### 3. Build

```bash
cmake --build --preset conan-debug
```

### 4. Symlink LSP

```bash
ln -sf build/Debug/compile_commands.json compile_commands.json
```

### 5. Run

```bash
./build/Debug/bin/GameEngine
```

## Re-Building

### Standard code changes

See optional aliases

```bash
cmake --build --preset conan-debug && ./build/Debug/bin/GameEngine
```

### If `conanfile.txt` changes

```bash
conan install . --build=missing -s build_type=Debug
cmake --preset conan-debug
```

### If `CMakeLists.txt` changes

```bash
cmake --preset conan-debug
```

### If compiler/toolchain changes

```bash
conan profile detect --force
```

## Clean build

```bash
rm -rf build compile_commands.json

conan install . --build=missing -s build_type=Debug
cmake --preset conan-debug
ln -sf build/Debug/compile_commands.json compile_commands.json
cmake --build --preset conan-debug
```

## Leak Detection

```bash
./scripts/run_leaks.sh
```

## Optional

Add an alias command to run this shit easier. If you're not on zsh, first of all, how dare you.

```bash
echo "alias crun='cmake --build --preset conan-debug --target run'" >> ~/.zshrc
echo "alias crun-release='cmake --build --preset conan-release --target run'" >> ~/.zshrc
source ~/.zshrc
```

Then run with

```bash
crun
```
