# Voxel world

`VoxelWorld` is the runtime voxel terrain store. It is a `System` that manages
a sparse, streaming 3D grid of blocks and a fluid-simulated water layer. The
`VoxelTerrain` render module reads it to draw terrain; `IsometricWater` reads
it to draw the animated water surface. `VoxelWorld` is the data authority; it
does not own or drive any renderer.

## Construction and setup

Add `VoxelWorld` to a scene as a system. The constructor takes the block-layer
range (in block z-coordinates) and the horizontal stream radius in tiles:

```cpp
// Blocks at z in [−128, 128); stream a 48-tile radius around the camera.
auto& world = scene.addSystem<sfs::VoxelWorld>(
    -4 * sfs::kChunkSize,   // minBlockZ
     16 * sfs::kChunkSize,  // maxBlockZ
     48                     // radiusTiles (default 48)
);
world.setGenerator(&myGenerator);
world.setBlockRegistry(&myBlockRegistry);
```

Both `setGenerator` and `setBlockRegistry` should be called before the first
frame. See [Core voxel layer](../../core/voxel/index.md) for how to implement
`IVoxelGenerator` and `IBlockRegistry`.

## Streaming

`VoxelWorld` tracks the camera (the entity with a `CameraComponent`) and keeps
a square window of chunks loaded around it. Chunks that scroll outside the
window are dropped; new chunks on the leading edge are generated and loaded.
The loaded window is `2 * radiusTiles + 1` tiles wide in each horizontal
direction.

## Editing at runtime

```cpp
// Place or replace a block (only in loaded chunks; silent no-op otherwise).
world.setBlock(x, y, z, blockId);

// Fill a cell with water (clamped to free capacity; wakes the sim nearby).
world.addWater(x, y, z, sfs::kWaterFull);

// Read back the current water amount at a cell.
int amount = world.waterAt(x, y, z);
```

`setBlock` marks the affected chunk (and up to three neighbours whose faces
abut the edited block) dirty, so `VoxelTerrain` remeshes only what changed.

## Water simulation

The world runs a fixed-timestep fluid sim at 15 ticks per second (up to 4
ticks per frame to catch up). Cells woken by `addWater` or `setBlock` are
simulated until they settle. A fully settled region costs no CPU.

## Wiring to rendering

Register both render modules on your isometric render system:

```cpp
auto& terrain = renderSystem.withModule<sfs::VoxelTerrain>();
terrain.setWorld(world, myBlockRegistry);

auto& water = renderSystem.withModule<sfs::IsometricWater>();
water.setWaterSurfaceSource(&world);
water.setWaveStrength(1.0f);   // Gerstner 3D waves (default for voxel)
water.setRippleStrength(0.0f); // flat colour ripple off (default for voxel)
```

`VoxelTerrain` reports `providesTerrainGeometry() = true`, which suppresses
billboard terrain tiles exactly as `BlockGeometry` does. You should not
register both `VoxelTerrain` and `BlockGeometry` at the same time.

## Terrain surface integration

`VoxelWorld` implements `ITerrainSurfaceSource`, `ITerrainHeightSource`, and
`IWaterSurfaceSource`. The render system picks these up automatically when
`VoxelWorld` is a registered system, so sun shadows, actor standing elevation,
blood-decal fade (puddles on water), and terrain-height sampling all work
without extra wiring.

## Read more

- [Core voxel layer](../../core/voxel/index.md) — block types, chunk layout,
  the mesher, and the water simulation.
- [Render modules](../rendering/render-modules/index.md) — `VoxelTerrain` and
  `IsometricWater` in detail.
