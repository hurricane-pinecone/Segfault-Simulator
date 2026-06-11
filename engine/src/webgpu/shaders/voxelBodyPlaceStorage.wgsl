// Size-class storage allocation: for each slot a component was just reduced into
// (slotMeta count > 0), pop a storage block from the free-list and record the
// body's (offset, dim) in the descriptor. The extract writes there and the
// transform publishes it, so a body's voxels live in an allocated block rather
// than at the fixed slot*96^3. C2a uses a single 96^3 class.
@group(0) @binding(0) var<storage, read> slotMeta : array<i32>;
@group(0) @binding(1) var<storage, read_write> desc : array<u32>;
@group(0) @binding(2) var<storage, read_write> classFree : array<atomic<u32>>;
@group(0) @binding(3) var<uniform> placeU : vec4<u32>; // x=isSplit, y=parent slot

fn popBlock() -> u32 {
  let c = atomicSub(&classFree[0], 1u);
  if (c == 0u || c > MAXB) { atomicAdd(&classFree[0], 1u); return 0xFFFFFFFFu; }
  return atomicLoad(&classFree[c]);
}

@compute @workgroup_size(64)
fn placeStorage(@builtin(global_invocation_id) gid : vec3<u32>) {
  let s = gid.x;
  if (s >= MAXB) { return; }
  if (placeU.x != 0u && s == placeU.y) { return; } // split parent keeps its block
  if (slotMeta[s * 16u + 6u] <= 0) { return; } // no component placed here this op
  let blk = popBlock();
  if (blk == 0xFFFFFFFFu) { return; } // pool full -> keep prior descriptor
  desc[s * 2u + 0u] = blk * BODYVOX;
  desc[s * 2u + 1u] = u32(BODYDIM);
}
