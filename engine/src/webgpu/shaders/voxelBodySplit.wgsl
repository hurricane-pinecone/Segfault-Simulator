// Splits a carved body's connected components (labeled by voxelBodyLabel) into
// independent pool slots. Voxel-level analogue of the world fell: register roots
// to slots, reduce each component's AABB/CoM/footprint, extract the non-parent
// components into free slots (same body-local coords) and clear them from the
// parent grid. The parent keeps one component. slotMeta layout (16 i32/slot):
// [0..2] aabbMin, [3..5] aabbMax, [6] count, [7..9] CoM-sum, [10] count (dup for
// the placement check), [11..12] footprint x,z, [13] footprint count.
@group(0) @binding(0) var<storage, read_write> bodyVox : array<u32>;
@group(0) @binding(1) var<storage, read> bodyLabel : array<u32>;
@group(0) @binding(2) var<storage, read_write> rootSlot : array<u32>;
@group(0) @binding(3) var<storage, read_write> slotMeta : array<atomic<i32>>;
@group(0) @binding(4) var<storage, read_write> occupied : array<atomic<u32>>;
@group(0) @binding(5) var<storage, read_write> slotCount : array<atomic<u32>>;
@group(0) @binding(6) var<uniform> parentU : vec4<u32>; // x = parent slot

const DIM : i32 = BODYDIM;
const SLOTVOX : u32 = BODYVOX;
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
  atomicAdd(&slotMeta[base + 7u], x);
  atomicAdd(&slotMeta[base + 8u], y);
  atomicAdd(&slotMeta[base + 9u], z);
  atomicAdd(&slotMeta[base + 10u], 1);
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
  if (atomicLoad(&slotMeta[base + 6u]) == 0) { return; } // not a target this split
  let parent = parentU.x;
  if (slot == parent) { return; }
  let li = lIdx(lx, ly, lz);
  let lab = bodyLabel[li];
  let mine = lab != SENTINEL && rootSlot[lab] == slot;
  bodyVox[slot * SLOTVOX + li] = select(0u, bodyVox[parent * SLOTVOX + li], mine);
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
  if (slot < MAXB && slot != parent) { bodyVox[parent * SLOTVOX + li] = 0u; }
}
