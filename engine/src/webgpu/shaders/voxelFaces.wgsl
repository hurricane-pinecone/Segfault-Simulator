// Per-brick face occupancy masks, from static solidity (type == 1). For each of
// a brick's 6 boundary faces, an 8x8 = 64-bit mask (2 u32) of which boundary
// voxels are solid. Two face-adjacent bricks are connected iff their facing
// masks overlap. Layout: faceBuf[bi*12 + face*2 + word]. Faces: 0 -X, 1 +X,
// 2 -Y, 3 +Y, 4 -Z, 5 +Z. The in-plane bit index is u + v*8 using the two
// non-face-axis local coords, identically on opposing faces so the masks align.
@group(0) @binding(0) var<storage, read> voxels : array<u32>;
@group(0) @binding(1) var<storage, read_write> faceBuf : array<u32>;

var<workgroup> fm : array<atomic<u32>, 12>;

fn setBit(face : i32, bit : i32) {
  atomicOr(&fm[u32(face * 2 + bit / 32)], 1u << u32(bit % 32));
}

@compute @workgroup_size(8, 8, 1)
fn faces(@builtin(local_invocation_id) lloc : vec3<u32>,
         @builtin(local_invocation_index) lidx : u32,
         @builtin(workgroup_id) wid : vec3<u32>) {
  let bi = u32(i32(wid.x) + i32(wid.y) * BG + i32(wid.z) * BG * BG);
  let lx = i32(lloc.x);
  let ly = i32(lloc.y);
  if (lidx < 12u) { atomicStore(&fm[lidx], 0u); }
  workgroupBarrier();
  for (var lz = 0; lz < 8; lz = lz + 1) {
    let v = voxels[bi * 512u + u32(lx + ly * 8 + lz * 64)];
    // Solid only; water/air do not support. A detached voxel (bit 4) is a
    // severed blob -- exclude it so it stops anchoring the bricks it touches.
    if ((v & 3u) != 1u || (v & 0x10u) != 0u) { continue; }
    if (lx == 0) { setBit(0, ly + lz * 8); }
    if (lx == 7) { setBit(1, ly + lz * 8); }
    if (ly == 0) { setBit(2, lx + lz * 8); }
    if (ly == 7) { setBit(3, lx + lz * 8); }
    if (lz == 0) { setBit(4, lx + ly * 8); }
    if (lz == 7) { setBit(5, lx + ly * 8); }
  }
  workgroupBarrier();
  if (lidx < 12u) { faceBuf[bi * 12u + lidx] = atomicLoad(&fm[lidx]); }
}
