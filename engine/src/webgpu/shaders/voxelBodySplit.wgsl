// Splits a carved body's connected components (labeled by voxelBodyLabel) into
// independent pool slots. Voxel-level analogue of the world fell: register roots
// to slots, reduce each component's AABB/CoM/footprint, extract the non-parent
// components into free slots (same body-local coords) and clear them from the
// parent grid. The parent keeps one component. slotMeta layout (16 i32/slot):
// [0..2] aabbMin, [3..5] aabbMax, [6] count, [7..9] CoM-sum, [10] count (dup for
// the placement check), [11..12] footprint x,z, [13] footprint count.
@group(0) @binding(0) var<storage, read> carveHit : array<u32>; // [0]body? [1]slot
@group(0) @binding(1) var<storage, read> bodyLabel : array<u32>;
@group(0) @binding(2) var<storage, read_write> rootSlot : array<u32>;
@group(0) @binding(3) var<storage, read_write> slotMeta : array<atomic<i32>>;
@group(0) @binding(4) var<storage, read_write> occupied : array<atomic<u32>>;
@group(0) @binding(5) var<storage, read_write> slotCount : array<atomic<u32>>;
@group(0) @binding(6) var<uniform> parentU : vec4<u32>; // x = parent slot
@group(0) @binding(7) var<storage, read> materials : array<Material>;
// Sparse brickmap body voxels: read parent, allocate + write children, clear cells.
@group(0) @binding(20) var<storage, read_write> bodyBrickGrid : array<atomic<u32>>;
@group(0) @binding(21) var<storage, read_write> bodyBrickPool : array<u32>;
@group(0) @binding(22) var<storage, read_write> bodyBrickFree : array<atomic<u32>>;
fn bodyVoxLoad(slot : u32, x : i32, y : i32, z : i32) -> u32 {
  let bp = atomicLoad(&bodyBrickGrid[slot * BODYBRICKS + brickCell(x, y, z)]);
  if (bp == BRICK_EMPTY) { return 0u; }
  return bodyBrickPool[bp * 512u + brickLocal(x, y, z)];
}
fn bodyVoxClear(slot : u32, x : i32, y : i32, z : i32) {
  let bp = atomicLoad(&bodyBrickGrid[slot * BODYBRICKS + brickCell(x, y, z)]);
  if (bp == BRICK_EMPTY) { return; }
  bodyBrickPool[bp * 512u + brickLocal(x, y, z)] = 0u;
}
fn popBrick() -> u32 {
  let c = atomicSub(&bodyBrickFree[0], 1u);
  if (c == 0u || c > BRICK_POOL_BRICKS) { atomicAdd(&bodyBrickFree[0], 1u); return BRICK_EMPTY; }
  return atomicLoad(&bodyBrickFree[c]);
}
fn pushBrick(b : u32) {
  let oldc = atomicAdd(&bodyBrickFree[0], 1u);
  atomicStore(&bodyBrickFree[oldc + 1u], b);
}
fn bodyVoxStore(slot : u32, x : i32, y : i32, z : i32, v : u32) {
  let bc = slot * BODYBRICKS + brickCell(x, y, z);
  var bp = atomicLoad(&bodyBrickGrid[bc]);
  if (bp == BRICK_EMPTY) {
    let nb = popBrick();
    if (nb == BRICK_EMPTY) { return; }
    for (var i = 0u; i < 512u; i = i + 1u) { bodyBrickPool[nb * 512u + i] = 0u; }
    let r = atomicCompareExchangeWeak(&bodyBrickGrid[bc], BRICK_EMPTY, nb);
    if (r.exchanged) { bp = nb; } else { pushBrick(nb); bp = r.old_value; }
  }
  bodyBrickPool[bp * 512u + brickLocal(x, y, z)] = v;
}

const DIM : i32 = BODYDIM;
const SENTINEL : u32 = 0xFFFFFFFFu;

fn lIdx(x : i32, y : i32, z : i32) -> u32 { return u32(x + y * DIM + z * DIM * DIM); }

// Atomically grab a free body slot (GPU owns allocation). SENTINEL = pool full.
fn claimSlot() -> u32 {
  for (var s = 0u; s < MAXB; s = s + 1u) {
    if (atomicLoad(&occupied[s]) == 0u) {
      let r = atomicCompareExchangeWeak(&occupied[s], 0u, 1u);
      if (r.exchanged) { return s; }
    }
  }
  return SENTINEL;
}

@compute @workgroup_size(4, 4, 4)
fn registerRoots(@builtin(global_invocation_id) gid : vec3<u32>) {
  let x = i32(gid.x); let y = i32(gid.y); let z = i32(gid.z);
  if (x >= DIM || y >= DIM || z >= DIM) { return; }
  let li = lIdx(x, y, z);
  // Only split the body THIS frame's carve actually hit: carveHit is the GPU's
  // truth, while the CPU's parentU is a one-frame-stale readback. On a mismatch
  // (world hit, or a different body) clear the root so reduce/extract/clearParent
  // all no-op -- this is the gate; the rest of the passes follow rootSlot.
  if (carveHit[0] != 1u || carveHit[1] != parentU.x) {
    rootSlot[li] = SENTINEL;
    return;
  }
  if (bodyLabel[li] == li) { // a component root (min voxel index of its component)
    let k = atomicAdd(&slotCount[0], 1u);
    // The first-registered component stays in the parent slot; the rest claim
    // fresh slots from the GPU occupancy bitmap. (Branch, not select() -- select
    // evaluates both arms and would claim a slot even for the parent.)
    if (k == 0u) {
      rootSlot[li] = parentU.x;
    } else {
      rootSlot[li] = claimSlot();
    }
  } else {
    rootSlot[li] = SENTINEL;
  }
}

@compute @workgroup_size(4, 4, 4)
fn reduce(@builtin(global_invocation_id) gid : vec3<u32>) {
  let x = i32(gid.x); let y = i32(gid.y); let z = i32(gid.z);
  if (x >= DIM || y >= DIM || z >= DIM) { return; }
  let li = lIdx(x, y, z);
  let lab = bodyLabel[li];
  if (lab == SENTINEL) { return; }
  let slot = rootSlot[lab];
  if (slot >= MAXB) { return; }
  let base = slot * 16u;
  atomicMin(&slotMeta[base + 0u], x);
  atomicMin(&slotMeta[base + 1u], y);
  atomicMin(&slotMeta[base + 2u], z);
  atomicMax(&slotMeta[base + 3u], x + 1);
  atomicMax(&slotMeta[base + 4u], y + 1);
  atomicMax(&slotMeta[base + 5u], z + 1);
  atomicAdd(&slotMeta[base + 6u], 1);
  // Mass-weighted CoM + second moment (weight each voxel by material density).
  let d = max(1, i32(materials[matId(bodyVoxLoad(parentU.x, x, y, z))].density * 16.0));
  atomicAdd(&slotMeta[base + 7u], x * d);
  atomicAdd(&slotMeta[base + 8u], y * d);
  atomicAdd(&slotMeta[base + 9u], z * d);
  atomicAdd(&slotMeta[base + 10u], d);
  atomicAdd(&slotMeta[base + 11u], (x * x / 16) * d);
  atomicAdd(&slotMeta[base + 12u], (y * y / 16) * d);
  atomicAdd(&slotMeta[base + 13u], (z * z / 16) * d);
}

@compute @workgroup_size(4, 4, 4)
fn footprint(@builtin(global_invocation_id) gid : vec3<u32>) {
  let x = i32(gid.x); let y = i32(gid.y); let z = i32(gid.z);
  if (x >= DIM || y >= DIM || z >= DIM) { return; }
  let li = lIdx(x, y, z);
  let lab = bodyLabel[li];
  if (lab == SENTINEL) { return; }
  let slot = rootSlot[lab];
  if (slot >= MAXB) { return; }
  let base = slot * 16u;
  // Bottom slab of THIS component (its own lowest rows), for the topple pivot.
  if (y < atomicLoad(&slotMeta[base + 1u]) + 4) {
    atomicAdd(&slotMeta[base + 11u], x);
    atomicAdd(&slotMeta[base + 12u], z);
    atomicAdd(&slotMeta[base + 13u], 1);
  }
}

// Dispatched over MAXB stacked DIM^3 grids (slot = gid.z / DIM). A child slot
// writes EVERY local cell -- the moved voxel where this component lives, else 0 --
// so stale geometry from the slot's previous tenant is wiped (else the freed
// slot's old body shows through as a phantom). Guarded on count != 0 so unrelated
// active bodies (count 0 this split) are never touched. The parent keeps its
// cells here; clearParent removes the moved-out ones in a following pass.
@compute @workgroup_size(4, 4, 4)
fn extract(@builtin(global_invocation_id) gid : vec3<u32>) {
  let slot = gid.z / u32(DIM);
  if (slot >= MAXB) { return; }
  let lx = i32(gid.x); let ly = i32(gid.y); let lz = i32(gid.z % u32(DIM));
  if (lx >= DIM || ly >= DIM) { return; }
  let base = slot * 16u;
  let count = atomicLoad(&slotMeta[base + 6u]);
  if (count == 0) { return; } // not a target this split
  let parent = parentU.x;
  if (slot == parent) { return; }
  // Sub-threshold fragment: not worth a rigid body (a swarm of 1-voxel leaf bits
  // when a falling tree is carved). Don't extract it into a slot -- clearParent
  // drops it from the parent and placeStorage/placeBodies skip it, so it just
  // disappears rather than spawning a tiny RB.
  if (count < MIN_BODY_VOXELS) { return; }
  let li = lIdx(lx, ly, lz);
  let lab = bodyLabel[li];
  let mine = lab != SENTINEL && rootSlot[lab] == slot;
  // Copy the moved voxel into the child; clear every other cell so a recycled
  // slot's previous tenant can't show through (the brick grid may hold stale
  // bricks until the reap frees them).
  if (mine) {
    bodyVoxStore(slot, lx, ly, lz, bodyVoxLoad(parent, lx, ly, lz));
  } else {
    bodyVoxClear(slot, lx, ly, lz);
  }
}

// Clear the parent's cells that moved into a child (runs after extract has copied
// them, so the read above is never racing this write).
@compute @workgroup_size(4, 4, 4)
fn clearParent(@builtin(global_invocation_id) gid : vec3<u32>) {
  let x = i32(gid.x); let y = i32(gid.y); let z = i32(gid.z);
  if (x >= DIM || y >= DIM || z >= DIM) { return; }
  let li = lIdx(x, y, z);
  let lab = bodyLabel[li];
  if (lab == SENTINEL) { return; }
  let slot = rootSlot[lab];
  let parent = parentU.x;
  if (slot < MAXB && slot != parent) { bodyVoxClear(parent, x, y, z); }
}
