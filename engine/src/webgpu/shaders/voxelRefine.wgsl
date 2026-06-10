// Voxel-exact refinement of the coarse brick anchoring, at the carve boundary.
// The coarse layer treats a brick as one node, so a brick the carve split into
// two blobs (a still-grounded stump + a severed stub) is wrongly kept whole. For
// each brick the carve touched (dirty), run a voxel BFS in workgroup memory:
// seed from solid voxels that touch a ground-anchored neighbour brick (trusted
// inflow) or the world floor, flood intra-brick solid connectivity, and flag any
// solid voxel not reached as detached (bit 4) -- in both voxel buffers, so it
// survives the ping-pong. Only dirty bricks are touched, so bit 4 stays exact.
@group(0) @binding(0) var<storage, read_write> vox0 : array<u32>;
@group(0) @binding(1) var<storage, read_write> vox1 : array<u32>;
@group(0) @binding(2) var<storage, read> anchor : array<u32>;
@group(0) @binding(3) var<storage, read_write> dirty : array<atomic<u32>>;

var<workgroup> reached : array<atomic<u32>, 16>;

fn bitGet(idx : i32) -> bool {
  return (atomicLoad(&reached[idx / 32]) & (1u << u32(idx % 32))) != 0u;
}
fn bitSet(idx : i32) { atomicOr(&reached[idx / 32], 1u << u32(idx % 32)); }

fn vIndex(x : i32, y : i32, z : i32) -> u32 {
  let bi = (x / 8) + (y / 8) * BG + (z / 8) * BG * BG;
  return u32(bi) * 512u + u32((x % 8) + (y % 8) * 8 + (z % 8) * 64);
}
fn solidAt(x : i32, y : i32, z : i32) -> bool {
  if (x < 0 || y < 0 || z < 0 || x >= WG || y >= WG || z >= WG) { return false; }
  return (vox0[vIndex(x, y, z)] & 3u) == 1u;
}
fn anchoredBrick(x : i32, y : i32, z : i32) -> bool {
  if (x < 0 || y < 0 || z < 0 || x >= WG || y >= WG || z >= WG) { return false; }
  return anchor[u32((x / 8) + (y / 8) * BG + (z / 8) * BG * BG)] == 1u;
}

@compute @workgroup_size(8, 8, 1)
fn refine(@builtin(local_invocation_id) lloc : vec3<u32>,
          @builtin(local_invocation_index) lidx : u32,
          @builtin(workgroup_id) wid : vec3<u32>) {
  let bi = u32(i32(wid.x) + i32(wid.y) * BG + i32(wid.z) * BG * BG);
  if (atomicLoad(&dirty[bi]) == 0u) { return; } // uniform across the workgroup

  let lx = i32(lloc.x);
  let ly = i32(lloc.y);
  let ox = i32(wid.x) * 8;
  let oy = i32(wid.y) * 8;
  let oz = i32(wid.z) * 8;

  if (lidx < 16u) { atomicStore(&reached[lidx], 0u); }
  workgroupBarrier();

  // Seed: a solid voxel is anchored if it sits on the world floor, or it touches
  // (through a shared face) a solid voxel in a ground-anchored neighbour brick.
  for (var lz = 0; lz < 8; lz = lz + 1) {
    let wx = ox + lx;
    let wy = oy + ly;
    let wz = oz + lz;
    if (!solidAt(wx, wy, wz)) { continue; }
    // Gravity model: support comes from the floor, below, or sideways -- NOT
    // from the brick above (+y). Else a cut brick's severed upper blob gets
    // seeded by the (wrongly) still-anchored brick above and is never detected.
    var seed = wy == 0;
    if (lx == 0 && solidAt(wx - 1, wy, wz) && anchoredBrick(wx - 1, wy, wz)) { seed = true; }
    if (lx == 7 && solidAt(wx + 1, wy, wz) && anchoredBrick(wx + 1, wy, wz)) { seed = true; }
    if (ly == 0 && solidAt(wx, wy - 1, wz) && anchoredBrick(wx, wy - 1, wz)) { seed = true; }
    if (lz == 0 && solidAt(wx, wy, wz - 1) && anchoredBrick(wx, wy, wz - 1)) { seed = true; }
    if (lz == 7 && solidAt(wx, wy, wz + 1) && anchoredBrick(wx, wy, wz + 1)) { seed = true; }
    if (seed) { bitSet(lx + ly * 8 + lz * 64); }
  }
  workgroupBarrier();

  // Flood intra-brick solid connectivity from the seeds (8^3 diameter <= 22).
  for (var round = 0; round < 22; round = round + 1) {
    for (var lz = 0; lz < 8; lz = lz + 1) {
      let idx = lx + ly * 8 + lz * 64;
      if (!solidAt(ox + lx, oy + ly, oz + lz) || bitGet(idx)) { continue; }
      var r = false;
      if (lx > 0 && bitGet(idx - 1)) { r = true; }
      if (lx < 7 && bitGet(idx + 1)) { r = true; }
      if (ly > 0 && bitGet(idx - 8)) { r = true; }
      if (ly < 7 && bitGet(idx + 8)) { r = true; }
      if (lz > 0 && bitGet(idx - 64)) { r = true; }
      if (lz < 7 && bitGet(idx + 64)) { r = true; }
      if (r) { bitSet(idx); }
    }
    workgroupBarrier();
  }

  // A solid voxel that the flood never reached is severed -> detached (bit 4),
  // written to both buffers so it persists across the ping-pong.
  for (var lz = 0; lz < 8; lz = lz + 1) {
    let vi = bi * 512u + u32(lx + ly * 8 + lz * 64);
    let detached =
        (vox0[vi] & 3u) == 1u && !bitGet(lx + ly * 8 + lz * 64);
    if (detached) {
      vox0[vi] = vox0[vi] | 0x10u;
      vox1[vi] = vox1[vi] | 0x10u;
    } else {
      vox0[vi] = vox0[vi] & ~0x10u;
      vox1[vi] = vox1[vi] & ~0x10u;
    }
  }

  if (lidx == 0u) { atomicStore(&dirty[bi], 0u); } // consumed
}
