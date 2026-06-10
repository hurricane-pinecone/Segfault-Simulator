// Voxel-exact world fell: connected-components + extract over the detached set
// that voxelWindowAnchor flagged (bit 4) inside the DIM^3 window around a carve.
// Mirrors the brick-level register/reduce/extract but at voxel resolution, so a
// felled piece starts at its true lowest detached voxel (no brick-snap). The CC
// runs in window-local coords; reduce records WORLD-coord AABBs so the CPU
// placement (onSlotMetaMapped, non-split branch) is unchanged. extract copies a
// component's voxels into its body slot, clears them from both world buffers, and
// accumulates the body-local CoM + contact footprint.
@group(0) @binding(0) var<storage, read_write> vox0 : array<u32>;
@group(0) @binding(1) var<storage, read_write> vox1 : array<u32>;
@group(0) @binding(2) var<storage, read> bricks : array<Brick>;
@group(0) @binding(3) var<storage, read> carveHit : array<u32>; // [2..4]=hit voxel
@group(0) @binding(4) var<storage, read> labelIn : array<u32>;
@group(0) @binding(5) var<storage, read_write> labelOut : array<u32>;
@group(0) @binding(6) var<storage, read_write> rootSlot : array<u32>;
@group(0) @binding(7) var<storage, read_write> slotMeta : array<atomic<i32>>;
@group(0) @binding(8) var<storage, read> freeSlot : array<u32>;
@group(0) @binding(9) var<storage, read_write> slotCount : array<atomic<u32>>;
@group(0) @binding(10) var<storage, read_write> bodyVox : array<u32>;

const DIM : i32 = 64;
const SLOTVOX : u32 = 262144u; // DIM^3
const SENTINEL : u32 = 0xFFFFFFFFu;

// Same window placement as voxelWindowAnchor (the cut near the bottom-centre).
fn winOrigin() -> vec3<i32> {
  let h = vec3<i32>(i32(carveHit[2]), i32(carveHit[3]), i32(carveHit[4]));
  let o = h - vec3<i32>(DIM / 2, 8, DIM / 2);
  return clamp(o, vec3<i32>(0), vec3<i32>(WG - DIM));
}
fn lIdx(x : i32, y : i32, z : i32) -> u32 { return u32(x + y * DIM + z * DIM * DIM); }
fn wIdx(x : i32, y : i32, z : i32) -> u32 {
  let bi = u32((x / 8) + (y / 8) * BG + (z / 8) * BG * BG);
  return bricks[bi].pointer * 512u + u32((x % 8) + (y % 8) * 8 + (z % 8) * 64);
}
fn detachedSolid(x : i32, y : i32, z : i32) -> bool {
  if (x < 0 || y < 0 || z < 0 || x >= WG || y >= WG || z >= WG) { return false; }
  let bi = u32((x / 8) + (y / 8) * BG + (z / 8) * BG * BG);
  if (bricks[bi].occupancy == 0u) { return false; }
  let v = vox0[wIdx(x, y, z)];
  return (v & 3u) == 1u && (v & 0x10u) != 0u;
}

@compute @workgroup_size(4, 4, 4)
fn labelInit(@builtin(global_invocation_id) gid : vec3<u32>) {
  let lx = i32(gid.x); let ly = i32(gid.y); let lz = i32(gid.z);
  if (lx >= DIM || ly >= DIM || lz >= DIM) { return; }
  let o = winOrigin();
  let li = lIdx(lx, ly, lz);
  labelOut[li] = select(SENTINEL, li, detachedSolid(o.x + lx, o.y + ly, o.z + lz));
}

fn nb(x : i32, y : i32, z : i32) -> u32 {
  if (x < 0 || y < 0 || z < 0 || x >= DIM || y >= DIM || z >= DIM) { return SENTINEL; }
  return labelIn[lIdx(x, y, z)]; // SENTINEL for air -> ignored by min
}

@compute @workgroup_size(4, 4, 4)
fn labelFlood(@builtin(global_invocation_id) gid : vec3<u32>) {
  let lx = i32(gid.x); let ly = i32(gid.y); let lz = i32(gid.z);
  if (lx >= DIM || ly >= DIM || lz >= DIM) { return; }
  let li = lIdx(lx, ly, lz);
  var lab = labelIn[li];
  if (lab == SENTINEL) { labelOut[li] = SENTINEL; return; }
  lab = min(lab, nb(lx - 1, ly, lz));
  lab = min(lab, nb(lx + 1, ly, lz));
  lab = min(lab, nb(lx, ly - 1, lz));
  lab = min(lab, nb(lx, ly + 1, lz));
  lab = min(lab, nb(lx, ly, lz - 1));
  lab = min(lab, nb(lx, ly, lz + 1));
  labelOut[li] = lab;
}

@compute @workgroup_size(4, 4, 4)
fn registerRoots(@builtin(global_invocation_id) gid : vec3<u32>) {
  let lx = i32(gid.x); let ly = i32(gid.y); let lz = i32(gid.z);
  if (lx >= DIM || ly >= DIM || lz >= DIM) { return; }
  let li = lIdx(lx, ly, lz);
  if (labelIn[li] == li) { // a component root (min voxel index of its component)
    let k = atomicAdd(&slotCount[0], 1u);
    rootSlot[li] = select(SENTINEL, freeSlot[1u + k], k < freeSlot[0]);
  } else {
    rootSlot[li] = SENTINEL;
  }
}

@compute @workgroup_size(4, 4, 4)
fn reduce(@builtin(global_invocation_id) gid : vec3<u32>) {
  let lx = i32(gid.x); let ly = i32(gid.y); let lz = i32(gid.z);
  if (lx >= DIM || ly >= DIM || lz >= DIM) { return; }
  let li = lIdx(lx, ly, lz);
  let lab = labelIn[li];
  if (lab == SENTINEL) { return; }
  let slot = rootSlot[lab];
  if (slot >= MAXB) { return; }
  let o = winOrigin();
  let wx = o.x + lx; let wy = o.y + ly; let wz = o.z + lz;
  let base = slot * 16u;
  atomicMin(&slotMeta[base + 0u], wx);
  atomicMin(&slotMeta[base + 1u], wy);
  atomicMin(&slotMeta[base + 2u], wz);
  atomicMax(&slotMeta[base + 3u], wx + 1);
  atomicMax(&slotMeta[base + 4u], wy + 1);
  atomicMax(&slotMeta[base + 5u], wz + 1);
  atomicAdd(&slotMeta[base + 6u], 1);
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
  let o = winOrigin();
  let cx = wx - o.x; let cy = wy - o.y; let cz = wz - o.z;
  if (cx < 0 || cy < 0 || cz < 0 || cx >= DIM || cy >= DIM || cz >= DIM) {
    bodyVox[bodyIdx] = 0u;
    return;
  }
  let v = vox0[wIdx(wx, wy, wz)];
  let lab = labelIn[lIdx(cx, cy, cz)];
  let mine = (v & 3u) == 1u && (v & 0x10u) != 0u && lab != SENTINEL &&
             rootSlot[lab] == slot;
  if (mine) {
    bodyVox[bodyIdx] = v & 0xFFFFFFEFu; // strip the detached flag in the body
    let vi = wIdx(wx, wy, wz);
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
  }
}
