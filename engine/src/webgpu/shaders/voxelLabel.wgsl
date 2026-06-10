// Brick-level connected-component labeling of the detached set (the chunks that
// fall when cut). A brick is detached iff it is NOT ground-anchored and has
// solid faces -- the same predicate as the body reduce (occupancy would scoop up
// water bricks). `labelInit` seeds each detached brick with its own index;
// `labelFlood` propagates the per-component minimum through solid-face-connected
// detached neighbours (same connectivity as voxelAnchor). Unlike anchoring there
// is no ground plane to converge against, so run many rounds (a min has to walk
// the whole component graph, one hop per round).
@group(0) @binding(0) var<storage, read> faceBuf : array<u32>;
@group(0) @binding(1) var<storage, read> anchor : array<u32>;
@group(0) @binding(2) var<storage, read> labelIn : array<u32>;
@group(0) @binding(3) var<storage, read_write> labelOut : array<u32>;

const SENTINEL : u32 = 0xFFFFFFFFu;

fn hasSolid(bi : u32) -> bool {
  var m = 0u;
  for (var k = 0u; k < 12u; k = k + 1u) { m = m | faceBuf[bi * 12u + k]; }
  return m != 0u;
}

fn detached(bi : u32) -> bool { return anchor[bi] == 0u && hasSolid(bi); }

fn overlap(a : u32, fa : i32, b : u32, fb : i32) -> bool {
  return ((faceBuf[a * 12u + u32(fa * 2 + 0)] & faceBuf[b * 12u + u32(fb * 2 + 0)]) |
          (faceBuf[a * 12u + u32(fa * 2 + 1)] & faceBuf[b * 12u + u32(fb * 2 + 1)])) != 0u;
}

@compute @workgroup_size(64)
fn labelInit(@builtin(global_invocation_id) gid : vec3<u32>) {
  let bi = gid.x;
  if (bi >= u32(BG) * u32(BG) * u32(BG)) { return; }
  labelOut[bi] = select(SENTINEL, bi, detached(bi));
}

// A connected detached neighbour's label, else SENTINEL (max u32 -> min ignores).
fn nbLabel(bi : u32, fa : i32, nb : u32, fb : i32) -> u32 {
  if (labelIn[nb] != SENTINEL && overlap(bi, fa, nb, fb)) { return labelIn[nb]; }
  return SENTINEL;
}

@compute @workgroup_size(64)
fn labelFlood(@builtin(global_invocation_id) gid : vec3<u32>) {
  let bi = gid.x;
  if (bi >= u32(BG) * u32(BG) * u32(BG)) { return; }
  var lab = labelIn[bi];
  if (lab == SENTINEL) { labelOut[bi] = SENTINEL; return; }
  let bx = i32(bi % u32(BG));
  let by = i32((bi / u32(BG)) % u32(BG));
  let bz = i32(bi / (u32(BG) * u32(BG)));
  if (bx > 0)      { lab = min(lab, nbLabel(bi, 0, bi - 1u, 1)); }
  if (bx < BG - 1) { lab = min(lab, nbLabel(bi, 1, bi + 1u, 0)); }
  if (by > 0)      { lab = min(lab, nbLabel(bi, 2, bi - u32(BG), 3)); }
  if (by < BG - 1) { lab = min(lab, nbLabel(bi, 3, bi + u32(BG), 2)); }
  if (bz > 0)      { lab = min(lab, nbLabel(bi, 4, bi - u32(BG) * u32(BG), 5)); }
  if (bz < BG - 1) { lab = min(lab, nbLabel(bi, 5, bi + u32(BG) * u32(BG), 4)); }
  labelOut[bi] = lab;
}
