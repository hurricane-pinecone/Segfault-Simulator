// Cleanup (every frame, after the water tick): recompute each brick's occupancy
// from the NEW buffer (water moved between bricks, so brick-skip needs it fresh)
// AND wipe just the dynamic (water) voxels from the OLD buffer, so it is ready to
// be written next frame. Static terrain is never cleared or copied -- it lives
// permanently in both buffers.
@group(0) @binding(0) var<storage, read> countBuf : array<u32>;       // new state
@group(0) @binding(1) var<storage, read_write> clearBuf : array<u32>; // old buffer
@group(0) @binding(2) var<storage, read_write> bricks : array<Brick>;
var<workgroup> occ : atomic<u32>;

@compute @workgroup_size(8, 8, 1)
fn recount(@builtin(local_invocation_id) lloc : vec3<u32>,
           @builtin(local_invocation_index) lidx : u32,
           @builtin(workgroup_id) wid : vec3<u32>) {
  let bi = u32(i32(wid.x) + i32(wid.y) * BG + i32(wid.z) * BG * BG);
  let slot = bricks[bi].pointer;
  let lx = i32(lloc.x);
  let ly = i32(lloc.y);
  if (lidx == 0u) { atomicStore(&occ, 0u); }
  workgroupBarrier();
  for (var lz = 0; lz < 8; lz = lz + 1) {
    let i = slot * 512u + u32(lx + ly * 8 + lz * 64);
    if ((clearBuf[i] & 3u) >= CAT_LIQUID) { clearBuf[i] = 0u; } // wipe dynamic (liquid+gas) from old buffer
    if ((countBuf[i] & 3u) != 0u) { atomicAdd(&occ, 1u); }  // occupancy of the new state
  }
  workgroupBarrier();
  if (lidx == 0u) { bricks[bi].occupancy = atomicLoad(&occ); }
}
