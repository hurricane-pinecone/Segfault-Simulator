# Game Engine

## Overview

This project uses **Conan (C++ package manager)** and **CMake** for building and dependency management.

---

## Initial Setup

Here's some instructions for Mac. If you're on anything else, you're on your own. Good luck.

### 1. Install dependencies

```bash
brew install conan cmake
conan profile detect --force
conan install . --build=missing -s build_type=Release
```

### 2. Configure project

```bash
cmake --preset conan-release
ln -sf build/Release/compile_commands.json compile_commands.json
```

### 3. Build

```bash
cmake --build --preset conan-release
```

### 4. Symlink LSP

```bash
ln -sf build/Release/compile_commands.json compile_commands.json
```

### 5. Run

```bash
./build/Release/bin/GameEngine
```

## Re-Building

### Standard code changes

```bash
cmake --build --preset conan-release && ./build/Release/bin/GameEngine
```

### If `conanfile.txt` changes

```bash
conan install . --build=missing -s build_type=Release
cmake --preset conan-release
```

### If `CMakeLists.txt` changes

```bash
cmake --preset conan-release
```

### If compiler/toolchain changes

```bash
conan profile detect --force
```

## Clean build

```bash
rm -rf build compile_commands.json

conan install . --build=missing -s build_type=Release
cmake --preset conan-release
ln -sf build/Release/compile_commands.json compile_commands.json
cmake --build --preset conan-release
```

## Optional

Add an alias command to run this shit easier.

```bash
echo "alias crun='cmake --build --preset conan-release --target run'" >> ~/.zshrc
```

Then run with

```bash
crun
```
