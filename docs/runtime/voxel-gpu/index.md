# GPU voxel world (WebGPU)

The `engine-webgpu` library adds a fully GPU-driven brickmap voxel world with
compute shader simulation and raymarched rendering. It is a separate CMake target
so only games that need it link `wgpu-native`; OpenGL games are unaffected.

## Linking engine-webgpu

In your game's `CMakeLists.txt`, link the alias alongside the base engine:

```cmake
target_link_libraries(myGame PRIVATE sfs::engine sfs::engine-webgpu)
```

## Setting up the game

The WebGPU path requires a `WebGpuRenderBackend` (see
[Render backends](../rendering/backends/index.md)). Override `makeRenderBackend()`
in your `Game` subclass:

```cpp
#include "engine/webgpu/webGpuRenderBackend.h"

std::unique_ptr<sfs::IRenderBackend> MyGame::makeRenderBackend()
{
  return std::make_unique<sfs::WebGpuRenderBackend>();
}
```

Retrieve the typed backend where you need it, for example in `onSetup()`:

```cpp
auto* backend = static_cast<sfs::WebGpuRenderBackend*>(renderBackend());
```

## Adding VoxelGpuSystem to a scene

`VoxelGpuSystem` (`engine/webgpu/voxel/voxelGpuSystem.h`) is the System that owns
and drives the GPU voxel world. Add it once to your scene, passing the backend:

```cpp
#include "engine/webgpu/voxel/voxelGpuSystem.h"

void MyScene::onInit() override
{
  auto* backend = static_cast<sfs::WebGpuRenderBackend*>(/* from game */);
  auto& voxels = addSystem<sfs::VoxelGpuSystem>(*backend);
}
```

The system generates terrain automatically on the first render frame.

## World constants

The world is a 512-voxel cube divided into 8-voxel bricks (a 64×64×64 brickmap):

| Constant | Value | Meaning |
| --- | --- | --- |
| `GpuVoxelWorld::kWorld` | 512 | World edge in voxels |
| `GpuVoxelWorld::kBrick` | 8 | Brick edge in voxels |
| `GpuVoxelWorld::kBG` | 64 | Brick-grid edge (kWorld / kBrick) |
| `GpuVoxelWorld::kBodyDim` | 96 | Local grid edge for one rigid body |
| `GpuVoxelWorld::kMaxBodies` | 256 | Rigid body pool size |

## Camera

`VoxelGpuSystem` owns an `OrbitCamera` that looks at the centre of the world from
a fixed elevation and orbits around it by yaw. Drive it from input each frame:

```cpp
// Rotate the view (e.g. Q / E keys or horizontal drag).
voxels.rotate(deltaYaw);

// Zoom via mouse wheel (positive scrollY moves closer).
voxels.zoom(scrollY);
```

## Click editing

Submit an edit at a screen pixel each frame:

```cpp
// mode 1 = carve a sphere, mode 2 = spawn water, 0 = no edit
voxels.setEdit(1, mouseX, mouseY);
```

Adjust the sphere radius:

```cpp
// Positive delta grows the radius; clamped to [0, 24].
voxels.adjustCarveRadius(delta);
```

When a carve severs an unsupported mass, the engine detects disconnected voxels
automatically and promotes them to falling rigid bodies.

## Rigid bodies

Severed chunks become rigid bodies that fall under gravity and topple when they
contact terrain. A body that comes to rest stamps its voxels back into the world.
A body that collides at high speed sheds voxels into the world as rubble. The body
pool holds up to `kMaxBodies` bodies simultaneously.

## Water simulation

Water is simulated as a cellular automaton on the GPU each frame. Spawn water by
calling `setEdit()` with mode 2; the water spreads and flows automatically without
any per-frame CPU involvement.

## GPU performance timestamps

If the device supports timestamp queries, the system records GPU timing each frame:

```cpp
if (voxels.hasTimestamps())
{
  double sim    = voxels.gpuSimMs();    // water CA + body physics passes
  double render = voxels.gpuRenderMs(); // raymarch pass
  double total  = voxels.gpuTotalMs();
}
```

## Debug overlays

Brick-grid and body-box wireframes are on by default. Toggle them at runtime:

```cpp
voxels.setDebugWire(false); // turn wireframes off
```

## Read more

- [Render backends](../rendering/backends/index.md) — `WebGpuRenderBackend` and
  `IRenderBackend` in detail.
- [Voxel world](../voxel/index.md) — the isometric CPU-driven voxel system; a
  separate path built on `sfs::engine`, not `engine-webgpu`.
