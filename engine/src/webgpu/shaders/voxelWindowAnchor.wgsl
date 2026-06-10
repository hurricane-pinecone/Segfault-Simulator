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

const DIM : i32 = 64;
const AIR : u32 = 0u;
const SOLID_UNREACHED : u32 = 1u;
const SOLID_REACHED : u32 = 2u;

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
  if (cur != SOLID_UNREACHED) { winOut[li] = cur; return; }
  let r = nbReached(lx - 1, ly, lz) || nbReached(lx + 1, ly, lz) ||
          nbReached(lx, ly - 1, lz) || nbReached(lx, ly + 1, lz) ||
          nbReached(lx, ly, lz - 1) || nbReached(lx, ly, lz + 1);
  winOut[li] = select(SOLID_UNREACHED, SOLID_REACHED, r);
}

@compute @workgroup_size(4, 4, 4)
fn winMark(@builtin(global_invocation_id) gid : vec3<u32>) {
  let lx = i32(gid.x); let ly = i32(gid.y); let lz = i32(gid.z);
  if (lx >= DIM || ly >= DIM || lz >= DIM) { return; }
  let o = winOrigin();
  let wx = o.x + lx; let wy = o.y + ly; let wz = o.z + lz;
  if (!worldSolid(wx, wy, wz)) { return; }
  let vi = wIdx(wx, wy, wz);
  if (winIn[lIdx(lx, ly, lz)] == SOLID_UNREACHED) {
    vox0[vi] = vox0[vi] | 0x10u;
    vox1[vi] = vox1[vi] | 0x10u;
  } else {
    vox0[vi] = vox0[vi] & ~0x10u;
    vox1[vi] = vox1[vi] & ~0x10u;
  }
}
