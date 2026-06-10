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
@group(0) @binding(4) var<storage, read> freeSlot : array<u32>;
@group(0) @binding(5) var<storage, read_write> slotCount : array<atomic<u32>>;
@group(0) @binding(6) var<uniform> parentU : vec4<u32>; // x = parent slot

const DIM : i32 = 64;
const SLOTVOX : u32 = 262144u;
const SENTINEL : u32 = 0xFFFFFFFFu;

fn lIdx(x : i32, y : i32, z : i32) -> u32 { return u32(x + y * DIM + z * DIM * DIM); }

@compute @workgroup_size(4, 4, 4)
fn registerRoots(@builtin(global_invocation_id) gid : vec3<u32>) {
  let x = i32(gid.x); let y = i32(gid.y); let z = i32(gid.z);
  if (x >= DIM || y >= DIM || z >= DIM) { return; }
  let li = lIdx(x, y, z);
  if (bodyLabel[li] == li) { // a component root (min voxel index of its component)
    let k = atomicAdd(&slotCount[0], 1u);
    rootSlot[li] = select(SENTINEL, freeSlot[1u + k], k < freeSlot[0]);
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

@compute @workgroup_size(4, 4, 4)
fn extract(@builtin(global_invocation_id) gid : vec3<u32>) {
  let x = i32(gid.x); let y = i32(gid.y); let z = i32(gid.z);
  if (x >= DIM || y >= DIM || z >= DIM) { return; }
  let li = lIdx(x, y, z);
  let lab = bodyLabel[li];
  if (lab == SENTINEL) { return; }
  let slot = rootSlot[lab];
  let parent = parentU.x;
  if (slot >= MAXB || slot == parent) { return; } // parent's component stays put
  // Move this voxel into its new slot's grid (same local coords) and clear it.
  let src = parent * SLOTVOX + li;
  bodyVox[slot * SLOTVOX + li] = bodyVox[src];
  bodyVox[src] = 0u;
}
