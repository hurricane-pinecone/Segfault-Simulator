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
@group(0) @binding(8) var<storage, read_write> occupied : array<atomic<u32>>;
@group(0) @binding(9) var<storage, read> materials : array<Material>;
// Sparse brickmap write path: lazily allocate an 8^3 brick on first write into a
// (slot, brick) cell, from a shared free-list.
@group(0) @binding(12) var<storage, read_write> wBrickGrid : array<atomic<u32>>;
@group(0) @binding(13) var<storage, read_write> wBrickPool : array<u32>;
@group(0) @binding(14) var<storage, read_write> wBrickFree : array<atomic<u32>>;
// Two-level CC (mirrors the anchor flood). detachedLocalCC labels each DETACHED
// voxel's within-brick component into voxLocalLabel; nodeLabel holds the min label
// per (brick, component). labelFlood pulls/pushes the node's min, so a component
// equalizes in ~one step instead of its voxel diameter -- but a cut keeps its two
// sides as separate components/nodes, so a severed piece gets its own label (its
// own body) rather than wrongly merging across the cut.
@group(0) @binding(15) var<storage, read_write> voxLocalLabel : array<u32>;
@group(0) @binding(16) var<storage, read_write> nodeLabel : array<atomic<u32>>;
// Per window-brick (12^3) flag: 1 if the brick holds any detached voxel. The node
// flood dispatches one workgroup per brick and skips the empty ones.
@group(0) @binding(17) var<storage, read_write> detBrick : array<u32>;
// Free-BITMAP allocator: wBrickFree[0] is a rotating alloc hint; wBrickFree[b+1]
// is 1 if brick b is free, 0 if used. Claim a free brick by CAS-ing its flag
// 1->0. This is race-free and leak-free -- unlike the old index-stack, two slots
// can never end up aliasing the same pool brick (the bug that wiped voxels from
// another brick on a fell).
fn popBrick() -> u32 {
  for (var k = 0u; k < BRICK_POOL_BRICKS; k = k + 1u) {
    let i = atomicAdd(&wBrickFree[0], 1u) % BRICK_POOL_BRICKS;
    if (atomicCompareExchangeWeak(&wBrickFree[i + 1u], 1u, 0u).exchanged) { return i; }
  }
  return BRICK_EMPTY; // pool full
}
fn pushBrick(b : u32) { atomicStore(&wBrickFree[b + 1u], 1u); }
fn bodyVoxStore(slot : u32, lx : i32, ly : i32, lz : i32, v : u32) -> u32 {
  let bc = slot * BODYBRICKS + brickCell(lx, ly, lz);
  var bp = atomicLoad(&wBrickGrid[bc]);
  if (bp == BRICK_EMPTY) {
    let nb = popBrick();
    if (nb == BRICK_EMPTY) { return BRICK_EMPTY; } // pool full -> caller keeps world voxel
    // No clear here: the reap zeroes bricks at free time (a frame earlier), so a
    // re-popped brick is already clean. Clearing here would RACE -- this non-atomic
    // 512-clear can land after another same-brick thread writes its voxel (via the
    // published pointer) and zero it back out.
    let r = atomicCompareExchangeWeak(&wBrickGrid[bc], BRICK_EMPTY, nb);
    if (r.exchanged) { bp = nb; } else { pushBrick(nb); bp = r.old_value; }
  }
  wBrickPool[bp * 512u + brickLocal(lx, ly, lz)] = v;
  return bp;
}

// Material density as a fixed-point weight (>=1) for the mass-weighted CoM and
// second moment, so light voxels (leaves) barely contribute to either. The scale
// cancels in CoM = sum/mass and inertia = E[r^2]-E[r]^2; keep it small so the
// pos^2 * weight accumulation stays in i32 range.
fn densW(v : u32) -> i32 { return max(1, i32(materials[matId(v)].density * 16.0)); }

// Atomically grab a free body slot (GPU owns allocation). SENTINEL = pool full.
fn claimSlot() -> u32 {
  for (var s = 0u; s < MAXB; s = s + 1u) {
    if (atomicLoad(&occupied[s]) == 0u) {
      let r = atomicCompareExchangeWeak(&occupied[s], 0u, 1u);
      if (r.exchanged) { return s; }
    }
  }
  return 0xFFFFFFFFu;
}

const DIM : i32 = BODYDIM;
const SLOTVOX : u32 = BODYVOX; // DIM^3
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

// Within-brick CC of the DETACHED set (one workgroup per window-brick, a z-column
// per thread, in shared memory). Each detached voxel ends labelled with its
// component's min LOCAL index (voxLocalLabel). Also seeds each (brick, component)
// node with its component's min GLOBAL (window-local) index for the node flood,
// and records whether the brick holds any detached voxel.
var<workgroup> lblD : array<u32, 512>;
var<workgroup> wgDet : atomic<u32>;
@compute @workgroup_size(64)
fn detachedLocalCC(@builtin(workgroup_id) wid : vec3<u32>,
                   @builtin(local_invocation_index) lidx : u32) {
  let o = winOrigin();
  let bx = i32(wid.x) * 8; let by = i32(wid.y) * 8; let bz = i32(wid.z) * 8;
  let brick = wid.x + wid.y * 12u + wid.z * 144u;
  let nodeBase = brick * 512u;
  let cx = i32(lidx % 8u); let cy = i32(lidx / 8u);
  if (lidx == 0u) { atomicStore(&wgDet, 0u); }
  for (var z = 0; z < 8; z = z + 1) {
    let li = u32(cx + cy * 8 + z * 64);
    lblD[li] = select(SENTINEL, li,
                      detachedSolid(o.x + bx + cx, o.y + by + cy, o.z + bz + z));
    atomicStore(&nodeLabel[nodeBase + li], SENTINEL);
  }
  workgroupBarrier();
  for (var it = 0; it < 24; it = it + 1) {
    for (var z = 0; z < 8; z = z + 1) {
      let li = u32(cx + cy * 8 + z * 64);
      var m = lblD[li];
      if (m != SENTINEL) {
        if (cx > 0) { m = min(m, lblD[li - 1u]); }
        if (cx < 7) { m = min(m, lblD[li + 1u]); }
        if (cy > 0) { m = min(m, lblD[li - 8u]); }
        if (cy < 7) { m = min(m, lblD[li + 8u]); }
        if (z > 0)  { m = min(m, lblD[li - 64u]); }
        if (z < 7)  { m = min(m, lblD[li + 64u]); }
        lblD[li] = m;
      }
    }
    workgroupBarrier();
  }
  // Publish the local component label, seed the node's min global index, and vote
  // the brick's detached flag. nodeLabel is reset above (barrier before this), so
  // the atomicMin seed is safe within the workgroup.
  for (var z = 0; z < 8; z = z + 1) {
    let li = u32(cx + cy * 8 + z * 64);
    let comp = lblD[li];
    voxLocalLabel[lIdx(bx + cx, by + cy, bz + z)] = comp;
    if (comp != SENTINEL) {
      atomicMin(&nodeLabel[nodeBase + comp], lIdx(bx + cx, by + cy, bz + z));
      atomicStore(&wgDet, 1u);
    }
  }
  workgroupBarrier();
  if (lidx == 0u) { detBrick[brick] = atomicLoad(&wgDet); }
}

// Node flood: one workgroup per window-brick (12^3), 64 threads scan the brick's
// 6 faces. For each detached face voxel whose outward neighbour (in the adjacent
// brick) is also detached, equalize the two (brick, component) nodes' min labels.
// Empty bricks skip entirely. Re-dispatched ~brick-diameter times; the in-place
// atomicMin makes it converge regardless of order.
fn conduct(brick : u32, wx : i32, wy : i32, wz : i32, ox : i32, oy : i32, oz : i32) {
  let comp = voxLocalLabel[lIdx(wx, wy, wz)];
  if (comp == SENTINEL) { return; } // this face voxel is not detached
  let nwx = wx + ox; let nwy = wy + oy; let nwz = wz + oz;
  if (nwx < 0 || nwy < 0 || nwz < 0 || nwx >= DIM || nwy >= DIM || nwz >= DIM) { return; }
  let nComp = voxLocalLabel[lIdx(nwx, nwy, nwz)];
  if (nComp == SENTINEL) { return; } // neighbour not detached -> no edge
  let nBrick = u32((nwx / 8) + (nwy / 8) * 12 + (nwz / 8) * 144);
  let myNode = brick * 512u + comp;
  let nNode = nBrick * 512u + nComp;
  let m = min(atomicLoad(&nodeLabel[myNode]), atomicLoad(&nodeLabel[nNode]));
  atomicMin(&nodeLabel[myNode], m);
  atomicMin(&nodeLabel[nNode], m);
}
@compute @workgroup_size(64)
fn nodeFlood(@builtin(workgroup_id) wid : vec3<u32>,
             @builtin(local_invocation_index) lidx : u32) {
  let brick = wid.x + wid.y * 12u + wid.z * 144u;
  if (detBrick[brick] == 0u) { return; } // no detached voxels here
  let bx = i32(wid.x) * 8; let by = i32(wid.y) * 8; let bz = i32(wid.z) * 8;
  let a = i32(lidx / 8u); let b = i32(lidx % 8u);
  conduct(brick, bx + 0, by + a, bz + b, -1, 0, 0);
  conduct(brick, bx + 7, by + a, bz + b, 1, 0, 0);
  conduct(brick, bx + a, by + 0, bz + b, 0, -1, 0);
  conduct(brick, bx + a, by + 7, bz + b, 0, 1, 0);
  conduct(brick, bx + a, by + b, bz + 0, 0, 0, -1);
  conduct(brick, bx + a, by + b, bz + 7, 0, 0, 1);
}

// Scatter each detached voxel's final node label back to the per-voxel label
// buffer the register/reduce/extract passes read; air -> SENTINEL.
@compute @workgroup_size(4, 4, 4)
fn scatter(@builtin(global_invocation_id) gid : vec3<u32>) {
  let lx = i32(gid.x); let ly = i32(gid.y); let lz = i32(gid.z);
  if (lx >= DIM || ly >= DIM || lz >= DIM) { return; }
  let li = lIdx(lx, ly, lz);
  let comp = voxLocalLabel[li];
  if (comp == SENTINEL) { labelOut[li] = SENTINEL; return; }
  labelOut[li] = atomicLoad(&nodeLabel[brickCell(lx, ly, lz) * 512u + comp]);
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
  // Node: pull the component's running min (instant intra-brick + mixed-brick
  // conduction), then push this voxel's label back into it.
  let node = brickCell(lx, ly, lz) * 512u + voxLocalLabel[li];
  let nl = atomicLoad(&nodeLabel[node]);
  if (nl != SENTINEL) { lab = min(lab, nl); }
  labelOut[li] = lab;
  atomicMin(&nodeLabel[node], lab);
}

@compute @workgroup_size(4, 4, 4)
fn registerRoots(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (carveHit[0] != 0u) { return; } // body hit -> split, not a world fell
  let lx = i32(gid.x); let ly = i32(gid.y); let lz = i32(gid.z);
  if (lx >= DIM || ly >= DIM || lz >= DIM) { return; }
  let li = lIdx(lx, ly, lz);
  if (labelIn[li] == li) { // a component root (min voxel index of its component)
    rootSlot[li] = claimSlot();
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
  if (carveHit[0] != 0u) { return; } // body hit -> split, not a world fell
  let slot = gid.z / u32(DIM);
  if (slot >= MAXB) { return; }
  let lx = i32(gid.x);
  let ly = i32(gid.y);
  let lz = i32(gid.z % u32(DIM));
  let base = slot * 16u;
  let count = atomicLoad(&slotMeta[base + 6u]);
  if (count == 0) { return; } // inactive slot (no voxels at all)
  // Below the minimum body size this component crumbles to powder rather than
  // spawning a tiny body. vox0 is the current src (bind group is src-indexed);
  // powder must land in src, so write it there and clear dst (vox1) -- writing both
  // would duplicate it.
  let cull = count < MIN_BODY_VOXELS;
  if (lx >= DIM || ly >= DIM || lz >= DIM) { return; }

  let wx = atomicLoad(&slotMeta[base + 0u]) + lx;
  let wy = atomicLoad(&slotMeta[base + 1u]) + ly;
  let wz = atomicLoad(&slotMeta[base + 2u]) + lz;
  let o = winOrigin();
  let cx = wx - o.x; let cy = wy - o.y; let cz = wz - o.z;
  if (cx < 0 || cy < 0 || cz < 0 || cx >= DIM || cy >= DIM || cz >= DIM) {
    return; // outside the carve window: never part of this body
  }
  let v = vox0[wIdx(wx, wy, wz)];
  let lab = labelIn[lIdx(cx, cy, cz)];
  let mine = (v & 3u) == 1u && (v & 0x10u) != 0u && lab != SENTINEL &&
             rootSlot[lab] == slot;
  if (cull) {
    if (mine) { // detached scrap below the body threshold: crumble to powder
      let vi = wIdx(wx, wy, wz);
      vox0[vi] = vox(matId(v), CAT_LIQUID) | VOX_POWDER; // src: falling rubble
      vox1[vi] = 0u;                                     // dst: cleared
    }
    return;
  }
  if (mine) {
    // A claimed slot's brick grid is clean (reaped or never used), so non-mine
    // cells need no clear -- only the moved voxel is written. Clear the world ONLY
    // if the store succeeded; a dropped voxel stays in the world (not vanished).
    let bp = bodyVoxStore(slot, lx, ly, lz, v & 0xFFFFFFEFu); // strip the detached flag
    if (bp == BRICK_EMPTY) { return; }
    let vi = wIdx(wx, wy, wz);
    vox0[vi] = 0u;
    vox1[vi] = 0u;
    let d = densW(v); // mass weight: CoM, mass, and second moment are all weighted
    atomicAdd(&slotMeta[base + 7u], lx * d);
    atomicAdd(&slotMeta[base + 8u], ly * d);
    atomicAdd(&slotMeta[base + 9u], lz * d);
    atomicAdd(&slotMeta[base + 10u], d);
    // sum d*pos^2 (scaled down by 16 to stay in i32 range; restored in place)
    atomicAdd(&slotMeta[base + 11u], (lx * lx / 16) * d);
    atomicAdd(&slotMeta[base + 12u], (ly * ly / 16) * d);
    atomicAdd(&slotMeta[base + 13u], (lz * lz / 16) * d);
  }
}
