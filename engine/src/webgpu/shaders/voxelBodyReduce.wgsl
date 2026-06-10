// Reduce the detached region to a voxel AABB + count, into the body-info buffer,
// before extraction. A brick is detached (its solids fall) when it is occupied
// but not ground-anchored. bodyMeta i32 layout: [0..2] aabbMin (init WG), [3..5]
// aabbMax (init 0), [6] count (init 0). (`meta` is a reserved WGSL word.)
@group(0) @binding(0) var<storage, read> anchor : array<u32>;
@group(0) @binding(1) var<storage, read> bricks : array<Brick>;
@group(0) @binding(2) var<storage, read_write> bodyMeta : array<atomic<i32>>;
@group(0) @binding(3) var<storage, read> faceBuf : array<u32>;

@compute @workgroup_size(64)
fn reduce(@builtin(global_invocation_id) gid : vec3<u32>) {
  let bi = gid.x;
  if (bi >= u32(BG) * u32(BG) * u32(BG)) { return; }
  if (anchor[bi] != 0u) { return; }
  // Only solid bricks are structural. Water-only bricks are occupied but never
  // anchored (the flood runs on solid faces), so gate on solid presence -- any
  // non-empty face mask -- not occupancy.
  var solidFaces = 0u;
  for (var k = 0u; k < 12u; k = k + 1u) { solidFaces = solidFaces | faceBuf[bi * 12u + k]; }
  if (solidFaces == 0u) { return; }
  let bx = i32(bi % u32(BG));
  let by = i32((bi / u32(BG)) % u32(BG));
  let bz = i32(bi / (u32(BG) * u32(BG)));
  atomicMin(&bodyMeta[0], bx * 8);
  atomicMin(&bodyMeta[1], by * 8);
  atomicMin(&bodyMeta[2], bz * 8);
  atomicMax(&bodyMeta[3], bx * 8 + 8);
  atomicMax(&bodyMeta[4], by * 8 + 8);
  atomicMax(&bodyMeta[5], bz * 8 + 8);
  atomicAdd(&bodyMeta[6], 1);
}
