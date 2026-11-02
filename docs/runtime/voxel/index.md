# Voxel world

`VoxelWorld` is the runtime voxel terrain store. It is a `System` that manages
a sparse, streaming grid of blocks and a fluid-simulated water layer. The
`VoxelModule` render module reads it to draw the terrain and its water.
`VoxelWorld` is the data authority; it does not own or drive any renderer.

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
abut the edited block) dirty, so `VoxelModule` remeshes only what changed.

## Water simulation

The world runs a fixed-timestep fluid sim at 15 ticks per second (up to 4
ticks per frame to catch up). Cells woken by `addWater` or `setBlock` are
simulated until they settle. A fully settled region costs no CPU.

## Wiring to rendering

`VoxelModule` draws the world in any style. In a 2D game it's placed through
the game's `Space2D` (an isometric game's grid, say); in 3D the grid is the
world's own, a block a unit, its levels up +Z. Register it with the asset
store, the world and the block registry:

```cpp
auto& render = addSystem<sfs::RenderSystem>(renderer());
render.setSpace2D(isometricSpace); // see Styles; leave it out for 3D
render.withModule<sfs::VoxelModule>(assetStore(), world, myBlockRegistry);
```

Each chunk's visible block faces, cut from its block types' sprites, are
meshed when the chunk streams in or changes and kept until then. Water draws
as see-through surfaces at each column's level; `setWaterColor(rgba)` sets its
colour and how see-through it is. The terrain is lit and shadowed like
everything else, and the depth buffer puts sprites in front of or behind its
blocks.

## Terrain surface integration

`VoxelWorld` implements `ITerrainSurfaceSource`, `ITerrainHeightSource`, and
`IWaterSurfaceSource`, so other parts of a game can use it as their terrain:

- `ParticleModule::enableStains(decals, &world)` lands particles on its
  ground, walls and water.
- `VoxelModule` reads its water columns to draw the water.

## Read more

- [Core voxel layer](../../core/voxel/index.md) — block types, chunk layout,
  the mesher, and the water simulation.
- [Render modules](../rendering/render-modules/index.md#voxelmodule): `VoxelModule`.
- [Styles](../rendering/styles/index.md): setting up an isometric or 3D game.
