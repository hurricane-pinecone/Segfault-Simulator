// Voxel-level ground anchoring in a bounded window around a carve, for a
// voxel-exact world fell (vs the brick-resolution flood). The window is a DIM^3
// box positioned so the cut sits near its bottom-centre. A window voxel is
// anchored if a solid path reaches a SEED: the world floor, or a window-boundary
// voxel whose neighbour JUST OUTSIDE the window is solid and in a brick the
// coarse flood anchored (i.e. it connects to the grounded bulk beyond the box).
// Solid voxels the flood never reaches are detached -> bit 4, voxel-exact at the
// cut. winInit seeds, winFlood propagates (ping-pong, run to convergence),
// winMark writes bit 4 into both world buffers.
@group(0) @binding(0) var<storage, read_write> vox0 : array<u32>;
@group(0) @binding(1) var<storage, read_write> vox1 : array<u32>;
@group(0) @binding(2) var<storage, read> bricks : array<Brick>;
@group(0) @binding(3) var<storage, read> anchor : array<u32>;
@group(0) @binding(4) var<storage, read> carveHit : array<u32>; // [2..4]=hit voxel
@group(0) @binding(5) var<storage, read> winIn : array<u32>;
@group(0) @binding(6) var<storage, read_write> winOut : array<u32>;
// Two-level acceleration (full-brick conduction). The window is DIM^3 = 12^3
// bricks; winBrickOcc holds each window-brick's world-solid count (FULL == 512),
// winBrickReach a monotonic per-brick reached flag. A FULL brick is internally
// fully connected, so reachability conducts across it (and between two adjacent
// full bricks) in one brick-step instead of 8 voxel-steps -- the dense grounded
// terrain the flood actually traverses is mostly full bricks, so this collapses
// the iteration count from the grid diameter (~96) to the brick diameter (~12).
@group(0) @binding(7) var<storage, read_write> winBrickOcc : array<u32>;
@group(0) @binding(8) var<storage, read_write> winBrickReach : array<atomic<u32>>;
// Mixed-brick conduction (the other half of the two-level). winLocalCC labels each
// solid voxel with its WITHIN-BRICK connected component (the min local index of
// that component, 0..511) into voxLocal. nodeReached is a per-(brick,component)
// monotonic reached flag: the moment any voxel of a component is reached, the
// whole component is (it is internally connected by construction) -- so a mixed
// brick (a leaf canopy, a cut plane) fills in ~one step instead of 8, and a CUT
// inside a brick keeps its two sides as SEPARATE components, so a severed tree
// side never gets reached (full-brick conduction can't do this -- it would treat
// the cut brick as one conducting node and wrongly re-attach the tree).
@group(0) @binding(9) var<storage, read_write> voxLocal : array<u32>;
@group(0) @binding(10) var<storage, read_write> nodeReached : array<atomic<u32>>;

const DIM : i32 = BODYDIM;
const AIR : u32 = 0u;
const SOLID_UNREACHED : u32 = 1u;
const SOLID_REACHED : u32 = 2u;
const WBD : u32 = 12u;          // window bricks per axis (DIM/8)
const BRICK_FULL : u32 = 512u;  // a fully-solid 8^3 brick
const LOCAL_SENTINEL : u32 = 0xFFFFFFFFu;

// The cut near the bottom-centre, so the falling piece (above it) fits the box.
fn winOrigin() -> vec3<i32> {
  let h = vec3<i32>(i32(carveHit[2]), i32(carveHit[3]), i32(carveHit[4]));
  let o = h - vec3<i32>(DIM / 2, 8, DIM / 2);
  return clamp(o, vec3<i32>(0), vec3<i32>(WG - DIM));
}

fn wIdx(x : i32, y : i32, z : i32) -> u32 {
  let bi = u32((x / 8) + (y / 8) * BG + (z / 8) * BG * BG);
  return bricks[bi].pointer * 512u + u32((x % 8) + (y % 8) * 8 + (z % 8) * 64);
}
fn worldSolid(x : i32, y : i32, z : i32) -> bool {
  if (x < 0 || y < 0 || z < 0 || x >= WG || y >= WG || z >= WG) { return false; }
  let bi = u32((x / 8) + (y / 8) * BG + (z / 8) * BG * BG);
  if (bricks[bi].occupancy == 0u) { return false; }
  return (vox0[wIdx(x, y, z)] & 3u) == 1u;
}
fn anchoredBrick(x : i32, y : i32, z : i32) -> bool {
  if (x < 0 || y < 0 || z < 0 || x >= WG || y >= WG || z >= WG) { return false; }
  return anchor[u32((x / 8) + (y / 8) * BG + (z / 8) * BG * BG)] == 1u;
}
fn lIdx(x : i32, y : i32, z : i32) -> u32 { return u32(x + y * DIM + z * DIM * DIM); }
fn winBrickCell(lx : i32, ly : i32, lz : i32) -> u32 {
  return u32(lx / 8) + u32(ly / 8) * WBD + u32(lz / 8) * WBD * WBD;
}

// Per window-brick world-solid count (one workgroup per brick, x/y threads loop
// z), so winFlood/winConduct know which bricks are FULL (conduct).
var<workgroup> occ : atomic<u32>;
@compute @workgroup_size(8, 8, 1)
fn winClassify(@builtin(workgroup_id) wid : vec3<u32>,
               @builtin(local_invocation_id) lloc : vec3<u32>,
               @builtin(local_invocation_index) lidx : u32) {
  if (lidx == 0u) { atomicStore(&occ, 0u); }
  workgroupBarrier();
  let o = winOrigin();
  let lx = i32(wid.x) * 8 + i32(lloc.x);
  let ly = i32(wid.y) * 8 + i32(lloc.y);
  for (var lz = 0; lz < 8; lz = lz + 1) {
    let wlz = i32(wid.z) * 8 + lz;
    if (worldSolid(o.x + lx, o.y + ly, o.z + wlz)) { atomicAdd(&occ, 1u); }
  }
  workgroupBarrier();
  if (lidx == 0u) {
    winBrickOcc[wid.x + wid.y * WBD + wid.z * WBD * WBD] = atomicLoad(&occ);
  }
}

// Conduct "reached" between adjacent FULL bricks (always connected). Monotonic
// (reached only sets), so in-place updates with read races are safe.
fn fullReached(bx : i32, by : i32, bz : i32) -> bool {
  if (bx < 0 || by < 0 || bz < 0 || bx >= i32(WBD) || by >= i32(WBD) || bz >= i32(WBD)) { return false; }
  let nb = u32(bx) + u32(by) * WBD + u32(bz) * WBD * WBD;
  return winBrickOcc[nb] == BRICK_FULL && atomicLoad(&winBrickReach[nb]) != 0u;
}
@compute @workgroup_size(64)
fn winConduct(@builtin(global_invocation_id) gid : vec3<u32>) {
  let b = gid.x;
  if (b >= WBD * WBD * WBD) { return; }
  if (winBrickOcc[b] != BRICK_FULL) { return; }    // only full bricks conduct
  if (atomicLoad(&winBrickReach[b]) != 0u) { return; } // already reached
  let bx = i32(b % WBD); let by = i32((b / WBD) % WBD); let bz = i32(b / (WBD * WBD));
  if (fullReached(bx - 1, by, bz) || fullReached(bx + 1, by, bz) ||
      fullReached(bx, by - 1, bz) || fullReached(bx, by + 1, bz) ||
      fullReached(bx, by, bz - 1) || fullReached(bx, by, bz + 1)) {
    atomicStore(&winBrickReach[b], 1u);
  }
}

// Within-brick connected-component labeling (one workgroup per window-brick, 64
// threads = one z-column each, in shared memory). Each solid voxel ends labelled
// with the min local index (0..511) of its in-brick component; voxLocal[windowVox]
// gets that label, so (brickCell, label) names a node for nodeReached.
var<workgroup> lbl : array<u32, 512>;
@compute @workgroup_size(64)
fn winLocalCC(@builtin(workgroup_id) wid : vec3<u32>,
              @builtin(local_invocation_index) lidx : u32) {
  let o = winOrigin();
  let bx = i32(wid.x) * 8; let by = i32(wid.y) * 8; let bz = i32(wid.z) * 8;
  let cx = i32(lidx % 8u); let cy = i32(lidx / 8u); // this thread's z-column
  for (var z = 0; z < 8; z = z + 1) {
    let li = u32(cx + cy * 8 + z * 64);
    let solid = worldSolid(o.x + bx + cx, o.y + by + cy, o.z + bz + z);
    lbl[li] = select(LOCAL_SENTINEL, li, solid);
  }
  workgroupBarrier();
  // Min-flood within the brick. 8^3 component diameter <= 24; 24 rounds converge.
  for (var it = 0; it < 24; it = it + 1) {
    for (var z = 0; z < 8; z = z + 1) {
      let li = u32(cx + cy * 8 + z * 64);
      var m = lbl[li];
      if (m != LOCAL_SENTINEL) { // air keeps SENTINEL; large -> ignored by min
        if (cx > 0) { m = min(m, lbl[li - 1u]); }
        if (cx < 7) { m = min(m, lbl[li + 1u]); }
        if (cy > 0) { m = min(m, lbl[li - 8u]); }
        if (cy < 7) { m = min(m, lbl[li + 8u]); }
        if (z > 0)  { m = min(m, lbl[li - 64u]); }
        if (z < 7)  { m = min(m, lbl[li + 64u]); }
        lbl[li] = m;
      }
    }
    workgroupBarrier();
  }
  // Publish the within-brick component label, and seed nodeReached for any
  // component that touches a ground seed (the world floor, or a boundary voxel
  // hanging off the coarse-anchored bulk -- the same seeds winInit used). The
  // node flood then conducts "reached" across the node graph.
  for (var z = 0; z < 8; z = z + 1) {
    let li = u32(cx + cy * 8 + z * 64);
    let comp = lbl[li];
    let gx = bx + cx; let gy = by + cy; let gz = bz + z; // window-local coords
    voxLocal[lIdx(gx, gy, gz)] = comp;
    if (comp != LOCAL_SENTINEL) {
      let wx = o.x + gx; let wy = o.y + gy; let wz = o.z + gz;
      var seed = wy == 0;
      if (gx == 0 && worldSolid(wx - 1, wy, wz) && anchoredBrick(wx - 1, wy, wz)) { seed = true; }
      if (gx == DIM - 1 && worldSolid(wx + 1, wy, wz) && anchoredBrick(wx + 1, wy, wz)) { seed = true; }
      if (gy == 0 && worldSolid(wx, wy - 1, wz) && anchoredBrick(wx, wy - 1, wz)) { seed = true; }
      if (gz == 0 && worldSolid(wx, wy, wz - 1) && anchoredBrick(wx, wy, wz - 1)) { seed = true; }
      if (gz == DIM - 1 && worldSolid(wx, wy, wz + 1) && anchoredBrick(wx, wy, wz + 1)) { seed = true; }
      if (seed) {
        atomicStore(&nodeReached[winBrickCell(gx, gy, gz) * 512u + comp], 1u);
      }
    }
  }
}

// Node-resolution reachability flood: one workgroup per window-brick (12^3),
// 64 threads scan the brick's 6 faces. If a solid face voxel's outward neighbour
// (in the adjacent brick) is also solid and EITHER (brick, component) node is
// reached, both are marked reached. Empty bricks skip. Monotonic OR-flood over the
// node graph -- replaces the full-grid winFlood + full-brick winConduct.
fn conductReach(brick : u32, wx : i32, wy : i32, wz : i32, ox : i32, oy : i32, oz : i32) {
  let comp = voxLocal[lIdx(wx, wy, wz)];
  if (comp == LOCAL_SENTINEL) { return; } // not solid
  let nwx = wx + ox; let nwy = wy + oy; let nwz = wz + oz;
  if (nwx < 0 || nwy < 0 || nwz < 0 || nwx >= DIM || nwy >= DIM || nwz >= DIM) { return; }
  let nComp = voxLocal[lIdx(nwx, nwy, nwz)];
  if (nComp == LOCAL_SENTINEL) { return; } // neighbour not solid -> no edge
  let nBrick = u32((nwx / 8) + (nwy / 8) * i32(WBD) + (nwz / 8) * i32(WBD) * i32(WBD));
  let myNode = brick * 512u + comp;
  let nNode = nBrick * 512u + nComp;
  if (atomicLoad(&nodeReached[myNode]) != 0u || atomicLoad(&nodeReached[nNode]) != 0u) {
    atomicStore(&nodeReached[myNode], 1u);
    atomicStore(&nodeReached[nNode], 1u);
  }
}
@compute @workgroup_size(64)
fn winNodeReachFlood(@builtin(workgroup_id) wid : vec3<u32>,
                     @builtin(local_invocation_index) lidx : u32) {
  let brick = wid.x + wid.y * WBD + wid.z * WBD * WBD;
  if (winBrickOcc[brick] == 0u) { return; } // empty brick
  let bx = i32(wid.x) * 8; let by = i32(wid.y) * 8; let bz = i32(wid.z) * 8;
  let a = i32(lidx / 8u); let b = i32(lidx % 8u);
  conductReach(brick, bx + 0, by + a, bz + b, -1, 0, 0);
  conductReach(brick, bx + 7, by + a, bz + b, 1, 0, 0);
  conductReach(brick, bx + a, by + 0, bz + b, 0, -1, 0);
  conductReach(brick, bx + a, by + 7, bz + b, 0, 1, 0);
  conductReach(brick, bx + a, by + b, bz + 0, 0, 0, -1);
  conductReach(brick, bx + a, by + b, bz + 7, 0, 0, 1);
}

@compute @workgroup_size(4, 4, 4)
fn winInit(@builtin(global_invocation_id) gid : vec3<u32>) {
  let lx = i32(gid.x); let ly = i32(gid.y); let lz = i32(gid.z);
  if (lx >= DIM || ly >= DIM || lz >= DIM) { return; }
  let o = winOrigin();
  let wx = o.x + lx; let wy = o.y + ly; let wz = o.z + lz;
  let li = lIdx(lx, ly, lz);
  if (!worldSolid(wx, wy, wz)) { winOut[li] = AIR; return; }
  // Seed: the world floor, or a boundary voxel whose just-outside neighbour is
  // solid and coarse-anchored (so it hangs off the grounded bulk beyond the box).
  // NOT the top face (+y): gravity model -- nothing above the box holds the
  // piece up, and the coarse flood above the box can be wrongly over-anchored.
  var seed = wy == 0;
  if (lx == 0 && worldSolid(wx - 1, wy, wz) && anchoredBrick(wx - 1, wy, wz)) { seed = true; }
  if (lx == DIM - 1 && worldSolid(wx + 1, wy, wz) && anchoredBrick(wx + 1, wy, wz)) { seed = true; }
  if (ly == 0 && worldSolid(wx, wy - 1, wz) && anchoredBrick(wx, wy - 1, wz)) { seed = true; }
  if (lz == 0 && worldSolid(wx, wy, wz - 1) && anchoredBrick(wx, wy, wz - 1)) { seed = true; }
  if (lz == DIM - 1 && worldSolid(wx, wy, wz + 1) && anchoredBrick(wx, wy, wz + 1)) { seed = true; }
  winOut[li] = select(SOLID_UNREACHED, SOLID_REACHED, seed);
}

fn nbReached(x : i32, y : i32, z : i32) -> bool {
  if (x < 0 || y < 0 || z < 0 || x >= DIM || y >= DIM || z >= DIM) { return false; }
  return winIn[lIdx(x, y, z)] == SOLID_REACHED;
}

@compute @workgroup_size(4, 4, 4)
fn winFlood(@builtin(global_invocation_id) gid : vec3<u32>) {
  let lx = i32(gid.x); let ly = i32(gid.y); let lz = i32(gid.z);
  if (lx >= DIM || ly >= DIM || lz >= DIM) { return; }
  let li = lIdx(lx, ly, lz);
  let cur = winIn[li];
  if (cur == AIR) { winOut[li] = AIR; return; }
  let bc = winBrickCell(lx, ly, lz);
  let full = winBrickOcc[bc] == BRICK_FULL;
  let node = bc * 512u + voxLocal[li]; // (brick, within-brick component)
  var r = cur == SOLID_REACHED; // already reached (seed or a prior round)
  if (!r) {
    r = nbReached(lx - 1, ly, lz) || nbReached(lx + 1, ly, lz) ||
        nbReached(lx, ly - 1, lz) || nbReached(lx, ly + 1, lz) ||
        nbReached(lx, ly, lz - 1) || nbReached(lx, ly, lz + 1);
    // Brick -> voxel: a reached FULL brick (carried by winConduct), or any reached
    // voxel of this voxel's component (carried by nodeReached), marks it reached.
    if (full && atomicLoad(&winBrickReach[bc]) != 0u) { r = true; }
    if (atomicLoad(&nodeReached[node]) != 0u) { r = true; }
  }
  winOut[li] = select(SOLID_UNREACHED, SOLID_REACHED, r);
  // Voxel -> {brick, component}: seed both flags so the conduction carries onward
  // (monotonic stores, race-safe). The component fill makes a mixed brick (canopy)
  // converge in ~one step; the cut keeps its two components as separate nodes.
  if (r) {
    atomicStore(&nodeReached[node], 1u);
    if (full) { atomicStore(&winBrickReach[bc], 1u); }
  }
}

@compute @workgroup_size(4, 4, 4)
fn winMark(@builtin(global_invocation_id) gid : vec3<u32>) {
  // carveHit[0] != 0 means the ray hit a rigid body, not the world: that frame
  // drives a body split, never a world fell. The CPU's fell/split choice uses a
  // one-frame-stale slot, so gate on this frame's GPU hit to avoid marking (and
  // later extracting) a phantom body from the body-local carve coords.
  if (carveHit[0] != 0u) { return; }
  let lx = i32(gid.x); let ly = i32(gid.y); let lz = i32(gid.z);
  if (lx >= DIM || ly >= DIM || lz >= DIM) { return; }
  let o = winOrigin();
  let wx = o.x + lx; let wy = o.y + ly; let wz = o.z + lz;
  if (!worldSolid(wx, wy, wz)) { return; }
  let vi = wIdx(wx, wy, wz);
  // Detached iff this voxel's (brick, component) node was never reached.
  let node = winBrickCell(lx, ly, lz) * 512u + voxLocal[lIdx(lx, ly, lz)];
  if (atomicLoad(&nodeReached[node]) == 0u) {
    vox0[vi] = vox0[vi] | 0x10u;
    vox1[vi] = vox1[vi] | 0x10u;
  } else {
    vox0[vi] = vox0[vi] & ~0x10u;
    vox1[vi] = vox1[vi] & ~0x10u;
  }
}
