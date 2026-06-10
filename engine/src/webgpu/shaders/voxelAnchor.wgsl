// Brick-level ground anchoring. A brick is anchored if it connects, through
// face-adjacent occupied bricks, down to a ground brick (brick-y 0). `seed` sets
// the ground bricks; `flood` propagates one monotone round (anchor only 0 -> 1).
// Run enough rounds (or until floodCtl reports no change) to converge.
@group(0) @binding(0) var<storage, read> faceBuf : array<u32>;
@group(0) @binding(1) var<storage, read> bricks : array<Brick>;
@group(0) @binding(2) var<storage, read> anchorIn : array<u32>;
@group(0) @binding(3) var<storage, read_write> anchorOut : array<u32>;
@group(0) @binding(4) var<storage, read_write> floodCtl : array<atomic<u32>>;

fn occupied(bi : u32) -> bool { return bricks[bi].occupancy != 0u; }

// Whether brick a's face `fa` overlaps neighbour b's opposing face `fb`.
fn overlap(a : u32, fa : i32, b : u32, fb : i32) -> bool {
  return ((faceBuf[a * 12u + u32(fa * 2 + 0)] & faceBuf[b * 12u + u32(fb * 2 + 0)]) |
          (faceBuf[a * 12u + u32(fa * 2 + 1)] & faceBuf[b * 12u + u32(fb * 2 + 1)])) != 0u;
}

fn nbAnchored(bi : u32, fa : i32, nb : u32, fb : i32) -> bool {
  return anchorIn[nb] == 1u && overlap(bi, fa, nb, fb);
}

@compute @workgroup_size(64)
fn seed(@builtin(global_invocation_id) gid : vec3<u32>) {
  let bi = gid.x;
  if (bi >= u32(BG) * u32(BG) * u32(BG)) { return; }
  let by = (bi / u32(BG)) % u32(BG);
  anchorOut[bi] = select(0u, 1u, occupied(bi) && by == 0u);
}

@compute @workgroup_size(64)
fn flood(@builtin(global_invocation_id) gid : vec3<u32>) {
  let bi = gid.x;
  if (bi >= u32(BG) * u32(BG) * u32(BG)) { return; }
  var a = anchorIn[bi];
  if (a == 0u && occupied(bi)) {
    let bx = i32(bi % u32(BG));
    let by = i32((bi / u32(BG)) % u32(BG));
    let bz = i32(bi / (u32(BG) * u32(BG)));
    if (bx > 0 && nbAnchored(bi, 0, bi - 1u, 1)) { a = 1u; }
    if (a == 0u && bx < BG - 1 && nbAnchored(bi, 1, bi + 1u, 0)) { a = 1u; }
    if (a == 0u && by > 0 && nbAnchored(bi, 2, bi - u32(BG), 3)) { a = 1u; }
    if (a == 0u && by < BG - 1 && nbAnchored(bi, 3, bi + u32(BG), 2)) { a = 1u; }
    if (a == 0u && bz > 0 && nbAnchored(bi, 4, bi - u32(BG) * u32(BG), 5)) { a = 1u; }
    if (a == 0u && bz < BG - 1 && nbAnchored(bi, 5, bi + u32(BG) * u32(BG), 4)) { a = 1u; }
    if (a == 1u) { atomicOr(&floodCtl[0], 1u); }
  }
  anchorOut[bi] = a;
}
