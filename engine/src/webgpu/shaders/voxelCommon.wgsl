// Shared prelude prepended to every voxel compute/render stage.
// Voxel u32 layout:
//   bits 0-1   category (0 air / 1 solid / 2 liquid) -- coarse class for sim dispatch
//   bits 2-3   liquid flow direction (0 +x, 1 -x, 2 +z, 3 -z)
//   bit  4     detached (rigid-body fell flag)
//   bits 8-15  material id (0 air, 1.. = entry in the material palette; gives colour,
//              density, rigidity, ... -- colour is NO LONGER baked per voxel)
//   bits 16-31 spare per-voxel CA state (temperature, lifetime, ...)
const WG : i32 = 512;
const BG : i32 = 64;
const SEA : i32 = 96;
const MAXB : u32 = 256u; // rigid-body pool size (must match kMaxBodies)
const BODYDIM : i32 = 96;       // rigid-body / fell-window grid edge (== kBodyDim)
const BODYVOX : u32 = 884736u;  // BODYDIM^3, one slot's grid
// A detached component smaller than this many voxels is NOT spawned as a rigid
// body -- it stays put as ordinary world solid. Caps the body-count explosion
// when a shed-holed canopy floods into a swarm of tiny disconnected leaf clumps.
const MIN_BODY_VOXELS : i32 = 32;
// Debug: 1 = draw each rigid body's bounding-box wireframe (magenta) as a
// non-occluding overlay; 0 = off.
const BODY_BOX : u32 = 1u;
// Sparse body storage: a body's 96^3 grid is a 12^3 grid of 8^3 bricks; each
// brick cell holds a pointer into a shared brick-voxel pool (512 voxels/brick) or
// BRICK_EMPTY. A body's brick cells start at slot*BODYBRICKS in the brick grid.
const BODYBD : i32 = 12;         // bricks per body axis (BODYDIM/8)
const BODYBRICKS : u32 = 1728u;  // BODYBD^3, brick cells per slot
const BRICK_EMPTY : u32 = 0xFFFFFFFFu;
fn brickCell(lx : i32, ly : i32, lz : i32) -> u32 {
  return u32((lx / 8) + (ly / 8) * BODYBD + (lz / 8) * BODYBD * BODYBD);
}
fn brickLocal(lx : i32, ly : i32, lz : i32) -> u32 {
  return u32((lx % 8) + (ly % 8) * 8 + (lz % 8) * 64);
}
struct Brick { occupancy : u32, pointer : u32 };
// Material palette entry (indexed by a voxel's material id). Must match the CPU
// material registry (gpuVoxelWorld material table).
struct Material {
  color : vec4<f32>,  // RGBA; a = opacity (transparent water etc.)
  density : f32,      // <1 rises, ~1 neutral, >1 sinks
  rigidity : f32,     // break-off resistance
  emission : f32,     // emissive strength (point lighting / glow)
  flags : u32,        // behaviour bits (reserved)
};
// Categories (voxel bits 0-1) and material ids (must match the CPU registry).
const CAT_AIR : u32 = 0u;
const CAT_SOLID : u32 = 1u;
const CAT_LIQUID : u32 = 2u;
const CAT_GAS : u32 = 3u;
const MAT_SAND : u32 = 1u;
const MAT_GRASS : u32 = 2u;
const MAT_DIRT : u32 = 3u;
const MAT_STONE : u32 = 4u;
const MAT_TRUNK : u32 = 5u;
const MAT_LEAVES : u32 = 6u;
const MAT_WATER : u32 = 7u;
const MAT_SMOKE : u32 = 8u;
const MAT_RUBBLE : u32 = 9u;
// Powder flag (bit 5): a dynamic voxel (liquid category, so it's cleaned/ping-
// ponged) that the fluid CA moves like a powder -- falls + slides diagonally down
// to pile -- instead of flowing flat. (A real 3-bit category is a future refactor.)
const VOX_POWDER : u32 = 0x20u;
fn matId(v : u32) -> u32 { return (v >> 8u) & 0xFFu; }
fn vox(matId : u32, cat : u32) -> u32 { return cat | (matId << 8u); }
// Body size classes (32^3 / 64^3 / 96^3). Counts MUST sum to MAXB and match the
// CPU pool sizing + free-list init. The pool is laid out small|med|large; the
// free-list is three [count, ids...] segments.
const CLS_NS : u32 = 128u;
const CLS_NM : u32 = 96u;
const CLS_NL : u32 = 32u;
const CLS_VS : u32 = 32768u;   // 32^3
const CLS_VM : u32 = 262144u;  // 64^3
const CLS_VL : u32 = 884736u;  // 96^3
const CLS_BM : u32 = CLS_NS * CLS_VS;          // med region base (voxels)
const CLS_BL : u32 = CLS_BM + CLS_NM * CLS_VM; // large region base
const CLS_FS : u32 = 0u;                        // small free-list segment
const CLS_FM : u32 = CLS_NS + 1u;               // med free-list segment
const CLS_FL : u32 = CLS_NS + 1u + CLS_NM + 1u; // large free-list segment
fn hash3(x : u32, y : u32, z : u32, f : u32) -> u32 {
  var h = x * 374761393u + y * 668265263u + z * 2246822519u + f * 3266489917u;
  h = (h ^ (h >> 13u)) * 1274126177u;
  return h ^ (h >> 16u);
}
