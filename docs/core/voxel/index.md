# Voxel terrain — core layer

The core voxel layer defines block types, chunk storage, the mesher that turns
chunks into drawable geometry, and the fluid simulation. It lives in the
engine-core library and has no dependency on rendering or the runtime, so
generators and water behavior can be unit-tested in isolation.

## Block types

`BlockType` is a plain data struct that describes one kind of block. Your game
creates these and registers them in an `IBlockRegistry`; the engine reads them
but never writes them.

```cpp
struct sfs::BlockType
{
  sfs::BlockShape       shape     = sfs::BlockShape::Cube;
  const std::string*    textureId = nullptr;   // pointer interned by your registry
  glm::vec4             uvRect    {0.0f, 0.0f, 1.0f, 1.0f};
  bool                  opaque    = true;
  bool                  solid     = true;
  sfs::SurfaceEffect::Type effect = sfs::SurfaceEffect::Type::None;
  bool                  liquid    = false;
};
```

`BlockId` is a `uint16_t`. `kAirBlock` (0) means empty. `kLevelsPerBlock` (2) is
the number of elevation levels one voxel cell spans, so a cube at z=0 occupies
levels 0 and 1, and a cube at z=1 occupies levels 2 and 3.

`BlockShape` is either `Cube` (fills the full cell) or `Slab` (fills only the
lower level). The mesher emits geometry for both shapes; the water simulation
tracks cell capacity accordingly.

Setting `liquid = true` marks a block that is physically present but emits no
opaque geometry. The mesher skips liquid blocks so solid terrain behind or below
the water stays visible. The water render path draws the animated surface instead.

## Block registry

`IBlockRegistry` maps a `BlockId` to its `BlockType`. Your game implements it
and passes the registry to `VoxelWorld` and to `VoxelTerrain`.

```cpp
class MyBlockRegistry : public sfs::IBlockRegistry
{
public:
  const sfs::BlockType& type(sfs::BlockId id) const override
  {
    return m_types.at(id);
  }

  sfs::BlockId add(sfs::BlockType bt)
  {
    auto id = static_cast<sfs::BlockId>(m_types.size());
    m_types.push_back(bt);
    return id;
  }

private:
  std::vector<sfs::BlockType> m_types{ {} }; // slot 0 is kAirBlock (unused)
};
```

## Voxel view

`IVoxelView` provides read access to block ids in world coordinates, across
chunk boundaries. The mesher samples neighbours through this interface so a
voxel on a chunk edge sees correctly into the adjacent chunk. `VoxelWorld`
implements it; in tests you can supply a minimal inline implementation.

## Chunk layout

A `VoxelChunk` is a dense `kChunkSize × kChunkSize × kChunkSize` (32³) grid of
`BlockId` values, laid out as `z * kChunkSize² + y * kChunkSize + x`.
All-air chunks are never allocated by `VoxelWorld`.

## Terrain generator

Implement `IVoxelGenerator` to supply world content. The engine calls
`generate()` once per chunk as terrain streams in, passing a chunk-local
`IChunkWriter`:

```cpp
class MyGenerator : public sfs::IVoxelGenerator
{
public:
  void generate(glm::ivec3 chunkCoord,
                sfs::IChunkWriter& out) const override
  {
    for (int lz = 0; lz < sfs::kChunkSize; ++lz)
    {
      const int wz = chunkCoord.z * sfs::kChunkSize + lz;
      if (wz >= 0) continue; // only fill below z = 0
      for (int ly = 0; ly < sfs::kChunkSize; ++ly)
        for (int lx = 0; lx < sfs::kChunkSize; ++lx)
          out.set(lx, ly, lz, m_stoneId);
    }
  }
private:
  sfs::BlockId m_stoneId = 1;
};
```

`IChunkWriter::setWater()` is optional. Call it to pre-fill water (oceans,
underground pools). Water placed by the generator is treated as already settled
and does not wake the simulation.

## Mesher

`meshChunk()` converts one chunk into a list of `VoxelMeshSlice` objects, one
per distinct material (texture and surface effect). The mesher emits only the
three camera-facing faces (`+x`, `+y`, `+z`) and culls faces whose neighbour is
opaque, so interior faces are never produced. It works in world space with no
projection; the `VoxelTerrain` render module projects the cached results to
screen each frame.

```cpp
std::vector<sfs::VoxelMeshSlice> slices =
    sfs::meshChunk(chunkCoord, voxelView, blockRegistry);
```

The mesher is pure: no GL or asset-store dependency, so it runs in any thread
or test environment.

## Water simulation

Water is a separate per-cell amount layer, not a block. The fluid tick
(`stepWater()`) is also pure and fully testable in isolation.

### Water storage

`WaterAmount` is a `uint16_t`. A full air cell holds `kWaterFull` (256) units.
A slab cell holds half (its lower level is solid). A cube cell holds zero.
`WaterChunk` stores one `WaterAmount` per cell, parallel to `VoxelChunk`.

### Fluid tick

`stepWater()` advances one tick over a set of active cells:

1. **Fall** — water moves straight down into any free capacity below it.
2. **Spread** — water equalises its surface height with same-z neighbours.

Cells are visited top-to-bottom, so a falling column drains in one pass.
Volume is exactly conserved; no cell can overfill. Cells that changed (plus
their immediate neighbours) populate `nextActive`. A fully settled region
produces an empty `nextActive`.

```cpp
std::vector<glm::ivec3> active = /* cells to simulate */;
std::vector<glm::ivec3> next;
sfs::stepWater(active, waterGrid, next);
// pass `next` as `active` on the following tick
```

`IWaterGrid` is the interface `stepWater` reads and writes. `VoxelWorld`
implements it. In tests a simple dense-array implementation is enough.

### Water surface source

`IWaterSurfaceSource` supplies the list of water columns for the water render
module (`IsometricWater`). `VoxelWorld` implements it automatically. You only
need to know about this interface if you build a custom water source that feeds
the same water render path.

## Read more

- [Voxel world](../../runtime/voxel/index.md) — the runtime streaming store
  built on these types.
- [Render modules](../../runtime/rendering/render-modules/index.md) — the
  `VoxelTerrain` and `IsometricWater` modules that draw a voxel world.
