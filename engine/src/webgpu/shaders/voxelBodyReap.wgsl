// GPU reap: retire every baked body slot in-place, with no CPU round-trip. Runs
// each frame right after the stamp pass (which has already written each baked
// body's voxels back into the world). For every active+baked slot it returns the
// slot's bricks to the free-list, releases the occupancy bit, and clears the
// state flag so next frame's step/prep see the slot as free. This replaces the
// CPU resting loop that drove freeing from a one-frame-stale mirror -- the
// allocation/freeing lifecycle is now entirely GPU-resident.
@group(0) @binding(0) var<storage, read_write> state : array<vec4<f32>>;
@group(0) @binding(3) var<storage, read_write> occupied : array<u32>;
// Sparse brickmap: return the slot's allocated bricks to the free-list and reset
// its grid cells to BRICK_EMPTY, so a later body claiming this slot starts clean.
@group(0) @binding(20) var<storage, read_write> bodyBrickGrid : array<u32>;
@group(0) @binding(22) var<storage, read_write> bodyBrickFree : array<atomic<u32>>;

@compute @workgroup_size(64)
fn reap(@builtin(global_invocation_id) gid : vec3<u32>) {
  let s = gid.x;
  if (s >= MAXB) { return; }
  let flags = bitcast<u32>(state[s * 8u + 0u].x);
  if ((flags & 1u) == 0u || (flags & 2u) == 0u) { return; } // active + baked only
  // Free this slot's bricks (one thread per slot, so no contention on its grid).
  let g0 = s * BODYBRICKS;
  for (var i = 0u; i < BODYBRICKS; i = i + 1u) {
    let bp = bodyBrickGrid[g0 + i];
    if (bp != BRICK_EMPTY) {
      let fc = atomicAdd(&bodyBrickFree[0], 1u);
      atomicStore(&bodyBrickFree[fc + 1u], bp);
      bodyBrickGrid[g0 + i] = BRICK_EMPTY;
    }
  }
  occupied[s] = 0u;
  state[s * 8u + 0u].x = 0.0; // clear flags -> slot free
}
