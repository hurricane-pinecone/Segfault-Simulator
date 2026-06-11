// Per-slot extract: dispatched over MAXB stacked DIM^3 grids (slot = gid.z / DIM).
// Slot s copies its component's solid voxels into bodyVox slot s (origin =
// slotMeta[s].aabbMin), clears them from BOTH world buffers, and accumulates the
// CoM + footprint. The rootSlot[label[brick]] == s test is load-bearing: two
// disjoint components can have overlapping AABBs, so a slot must only take/clear
// its OWN voxels.
@group(0) @binding(0) var<storage, read_write> vox0 : array<u32>;
@group(0) @binding(1) var<storage, read_write> vox1 : array<u32>;
@group(0) @binding(2) var<storage, read> anchor : array<u32>;
@group(0) @binding(3) var<storage, read_write> slotMeta : array<atomic<i32>>;
@group(0) @binding(4) var<storage, read_write> bodyVox : array<u32>;
@group(0) @binding(5) var<storage, read> labelBuf : array<u32>;
@group(0) @binding(6) var<storage, read> rootSlot : array<u32>;

const DIM : i32 = BODYDIM;
const SLOTVOX : u32 = BODYVOX; // DIM^3
const SENTINEL : u32 = 0xFFFFFFFFu;

fn vIndex(x : i32, y : i32, z : i32) -> u32 {
  let bi = (x / 8) + (y / 8) * BG + (z / 8) * BG * BG;
  return u32(bi) * 512u + u32((x % 8) + (y % 8) * 8 + (z % 8) * 64);
}

@compute @workgroup_size(4, 4, 4)
fn extract(@builtin(global_invocation_id) gid : vec3<u32>) {
  let slot = gid.z / u32(DIM);
  if (slot >= MAXB) { return; }
  let lx = i32(gid.x);
  let ly = i32(gid.y);
  let lz = i32(gid.z % u32(DIM));
  if (lx >= DIM || ly >= DIM) { return; }
  let base = slot * 16u;
  if (atomicLoad(&slotMeta[base + 6u]) == 0) { return; } // inactive slot
  let bodyIdx = slot * SLOTVOX + u32(lx + ly * DIM + lz * DIM * DIM);

  let wx = atomicLoad(&slotMeta[base + 0u]) + lx;
  let wy = atomicLoad(&slotMeta[base + 1u]) + ly;
  let wz = atomicLoad(&slotMeta[base + 2u]) + lz;
  if (wx < 0 || wy < 0 || wz < 0 || wx >= WG || wy >= WG || wz >= WG) {
    bodyVox[bodyIdx] = 0u;
    return;
  }
  let vi = vIndex(wx, wy, wz);
  let v = vox0[vi];
  let brick = u32((wx / 8) + (wy / 8) * BG + (wz / 8) * BG * BG);
  let lab = labelBuf[brick];
  let mine = lab != SENTINEL && rootSlot[lab] == slot;
  if ((v & 3u) == 1u && anchor[brick] == 0u && mine) {
    bodyVox[bodyIdx] = v & 0xFFFFFFEFu;
    vox0[vi] = 0u;
    vox1[vi] = 0u;
    atomicAdd(&slotMeta[base + 7u], lx);
    atomicAdd(&slotMeta[base + 8u], ly);
    atomicAdd(&slotMeta[base + 9u], lz);
    atomicAdd(&slotMeta[base + 10u], 1);
    if (ly < 8) {
      atomicAdd(&slotMeta[base + 11u], lx);
      atomicAdd(&slotMeta[base + 12u], lz);
      atomicAdd(&slotMeta[base + 13u], 1);
    }
  } else {
    bodyVox[bodyIdx] = 0u;
    // Clear a stale detached flag on stubs left in anchored bricks (never on an
    // unanchored voxel, which belongs to some slot's extraction).
    if ((v & 0x10u) != 0u && anchor[brick] != 0u) {
      vox0[vi] = v & 0xFFFFFFEFu;
      vox1[vi] = vox1[vi] & 0xFFFFFFEFu;
    }
  }
}
