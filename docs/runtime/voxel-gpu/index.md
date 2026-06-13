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
// mode 0 = no edit, 1 = carve a sphere, 2 = spawn water, 5 = ignite
voxels.setEdit(1, mouseX, mouseY);
```

| Mode | Effect |
| --- | --- |
| 0 | No edit |
| 1 | Carve a sphere |
| 2 | Spawn water |
| 5 | Ignite flammable voxels in a sphere |

Adjust the sphere radius:

```cpp
// Positive delta grows the radius; clamped to [0, 24].
voxels.adjustCarveRadius(delta);
```

When a carve or a fire burnthrough severs an unsupported mass, the engine detects
disconnected voxels automatically and promotes them to falling rigid bodies.

## Rigid bodies

Severed chunks become rigid bodies that fall under gravity and topple when they
contact terrain. A body that comes to rest stamps its voxels back into the world.
A body that collides at high speed sheds voxels into the world as rubble. The body
pool holds up to `kMaxBodies` bodies simultaneously.

A rigid body that is on fire continues burning in its own voxel grid. Burning body
voxels ignite adjacent world fuel, and adjacent burning world voxels can ignite body
voxels, so fire crosses the rigid-body/world boundary naturally.

## Water simulation

Water is simulated as a cellular automaton on the GPU each frame. Spawn water by
calling `setEdit()` with mode 2; the water spreads and flows automatically without
any per-frame CPU involvement.

## Fire simulation

Fire is a GPU cellular automaton that runs every frame on flammable voxels in the
world and on rigid bodies. No per-frame CPU work is needed once a voxel is ignited.

**Flammable materials** are those with a non-zero catch rate in the material
palette. The built-in flammable materials are grass, wood trunk, and leaves. Each
has tuned catch, burnout, and crumble rates:

| Material | Catch rate (per sec) | Burnout rate (per sec) | Crumble chance |
| --- | --- | --- | --- |
| Grass | 3.0 | 2.5 | 0.5 |
| Trunk | 4.0 | 0.18 | 0.8 |
| Leaves | 5.0 | 2.5 | 0.0 |

**Ignition.** A flammable voxel catches fire when any of its six face neighbours is
burning. The probability per frame is `catchRate * dt`. Use edit mode 5 to ignite
voxels at the cursor.

**Burnout.** A burning voxel has a `burnoutRate * dt` chance each frame of burning
out. The outcome depends on the material:

- Leaves burn to air and vanish.
- Wood trunk burns out with a `crumbleChance` probability of converting to falling
  char powder, and otherwise converts to a static char voxel. Because trunk has a
  high crumble chance and a low burnout rate, a burning trunk slowly hollows out
  from within and eventually causes its canopy to collapse.
- Burning powder is consumed to air.

**Char.** Char (`MAT_CHAR`) is the burnt-out remains of a flammable voxel. It is
not flammable itself. Char voxels with two or fewer solid neighbours crumble to
falling char powder, so a charred structure collapses progressively rather than
staying frozen in place.

**Structural collapse.** When fire burns through a load-bearing voxel (a trunk or
char), the engine re-runs the same flood-fill and fell pass it uses for carving.
Unsupported masses detach and become rigid bodies without any extra code.

**Smoke.** Burning voxels puff a smoke voxel into the air cell directly above them
at roughly three times per second.

**Rendering.** Burning voxels are rendered with a per-voxel flickering orange-to-
yellow self-lit colour. The flicker is driven by a hash of the voxel position and
the current frame number, so adjacent voxels flicker independently.

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
