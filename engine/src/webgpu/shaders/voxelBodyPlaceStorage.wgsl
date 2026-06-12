// Slot finalize after a reduce: cull sub-threshold world-fell components. Body
// voxel storage is the sparse brickmap, allocated lazily by the extract, so there
// is no per-slot block to reserve here -- this pass only releases the slots that
// won't become bodies.
@group(0) @binding(0) var<storage, read> slotMeta : array<i32>;
@group(0) @binding(3) var<uniform> placeU : vec4<u32>; // x=isSplit, y=parent slot
@group(0) @binding(4) var<storage, read_write> occupied : array<u32>;

@compute @workgroup_size(64)
fn placeStorage(@builtin(global_invocation_id) gid : vec3<u32>) {
  let s = gid.x;
  if (s >= MAXB) { return; }
  let count = slotMeta[s * 16u + 6u];
  if (count <= 0) { return; } // no component placed here this op
  // Sub-threshold component: no body. Release the slot the register pass claimed
  // so it is reusable -- the world-fell extract turns its voxels into powder; a
  // split fragment is dropped by clearParent. The split parent (placeU.y) is a
  // live body, so it is never culled.
  let isSplitParent = placeU.x != 0u && s == placeU.y;
  if (count < MIN_BODY_VOXELS && !isSplitParent) { occupied[s] = 0u; }
}
