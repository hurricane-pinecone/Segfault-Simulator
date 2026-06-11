// GPU reap: retire every baked body slot in-place, with no CPU round-trip. Runs
// each frame right after the stamp pass (which has already written each baked
// body's voxels back into the world). For every active+baked slot it returns the
// storage block to the free-list, releases the occupancy bit, and clears the
// state flag so next frame's step/prep see the slot as free. This replaces the
// CPU resting loop that drove freeing from a one-frame-stale mirror -- the
// allocation/freeing lifecycle is now entirely GPU-resident.
@group(0) @binding(0) var<storage, read_write> state : array<vec4<f32>>;
@group(0) @binding(1) var<storage, read> desc : array<u32>;
@group(0) @binding(2) var<storage, read_write> classFree : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read_write> occupied : array<u32>;

@compute @workgroup_size(64)
fn reap(@builtin(global_invocation_id) gid : vec3<u32>) {
  let s = gid.x;
  if (s >= MAXB) { return; }
  let flags = bitcast<u32>(state[s * 8u + 0u].x);
  if ((flags & 1u) == 0u || (flags & 2u) == 0u) { return; } // active + baked only
  let blk = desc[s * 2u] / BODYVOX;
  let oldc = atomicAdd(&classFree[0], 1u);
  atomicStore(&classFree[oldc + 1u], blk);
  occupied[s] = 0u;
  state[s * 8u + 0u].x = 0.0; // clear flags -> slot free
}
