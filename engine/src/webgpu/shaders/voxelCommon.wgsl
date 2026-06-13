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
// Debug wireframe overlays (body bounding box + 8^3 brick grid) are gated at
// runtime by the render's dbgMouse.z flag (toggled with the P key).
// Sparse body storage: a body's 96^3 grid is a 12^3 grid of 8^3 bricks; each
// brick cell holds a pointer into a shared brick-voxel pool (512 voxels/brick) or
// BRICK_EMPTY. A body's brick cells start at slot*BODYBRICKS in the brick grid.
const BODYBD : i32 = 12;         // bricks per body axis (BODYDIM/8)
const BODYBRICKS : u32 = 1728u;  // BODYBD^3, brick cells per slot
const BRICK_EMPTY : u32 = 0xFFFFFFFFu;
const BRICK_POOL_BRICKS : u32 = 16384u; // shared brick-pool capacity (== cpp)
fn brickCell(lx : i32, ly : i32, lz : i32) -> u32 {
  return u32((lx / 8) + (ly / 8) * BODYBD + (lz / 8) * BODYBD * BODYBD);
}
fn brickLocal(lx : i32, ly : i32, lz : i32) -> u32 {
  return u32((lx % 8) + (ly % 8) * 8 + (lz % 8) * 64);
}
// NOTE: brickmap body-voxel access bindings (grid/pool) are declared PER-SHADER,
// not here -- a binding declared in voxelCommon lands in every prepended shader's
// interface and breaks pipelines that don't provide it. brickCell/brickLocal +
// BODYBRICKS/BRICK_EMPTY above are pure (no bindings) and shared. Each reading
// pass declares bindings 20/21 + its own bodyVoxLoad; write passes add 22/23/24.
struct Brick { occupancy : u32, pointer : u32 };
// Material palette entry (indexed by a voxel's material id). Must match the CPU
// material registry (gpuVoxelWorld material table).
struct Material {
  color : vec4<f32>,  // RGBA; a = opacity (transparent water etc.)
  density : f32,      // <1 rises, ~1 neutral, >1 sinks
  rigidity : f32,     // break-off resistance
  emission : f32,     // emissive strength (point lighting / glow)
  flags : u32,        // behaviour bits (reserved)
  // Fire: x catchRate (per-sec spread/ignite, >0 = flammable), y burnoutRate
  // (per-sec chance to burn out, low = burns long), z crumbleChance (fall as
  // powder vs char on burnout), w spare.
  fire : vec4<f32>,
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
const MAT_CHAR : u32 = 10u; // charred remains: a burnt-out flammable voxel
// Fire: a flammable SOLID voxel can be BURNING (bit 6); while burning it counts a
// burn timer down in bits 16-23 (solids don't otherwise use those), spreads to
// flammable neighbours, then turns to char. Material flammability is palette flags
// bit 0. Fire behaviour is now palette-driven (Material.fire), so a burning voxel
// needs no timer -- it burns out probabilistically (burnoutRate). The catch/burnout
// rates live on the material; the CAs read them directly.
const VOX_BURNING : u32 = 0x40u;
fn isBurning(v : u32) -> bool { return (v & VOX_BURNING) != 0u; }
// Powder flag (bit 5): a dynamic voxel (liquid category, so it's cleaned/ping-
// ponged) that the fluid CA moves like a powder -- falls + slides diagonally down
// to pile -- instead of flowing flat. (A real 3-bit category is a future refactor.)
const VOX_POWDER : u32 = 0x20u;
fn matId(v : u32) -> u32 { return (v >> 8u) & 0xFFu; }
fn vox(matId : u32, cat : u32) -> u32 { return cat | (matId << 8u); }
// Ballistic debris: a flying material voxel (carries its full voxel value `vox`, so
// every per-material attribute -- density/rigidity/colour, flammability later --
// travels with it). A sparse pool: the blast ejects shell voxels as debris, the
// advect pass flies + collides them, and on impact/slow-down they settle back into
// the grid as powder. Ring-allocated (head % DEBRIS_MAX); life <= 0 = a free slot.
const DEBRIS_MAX : u32 = 16384u;
const DEBRIS_G : f32 = 80.0; // gravity, matches the rigid-body step
struct Debris {
  a : vec4<f32>, // pos.xyz, life (seconds; <= 0 dead)
  b : vec4<f32>, // vel.xyz, bitcast<f32>(voxel value)
};
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
