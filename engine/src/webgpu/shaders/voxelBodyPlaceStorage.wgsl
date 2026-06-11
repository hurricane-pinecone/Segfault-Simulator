// Size-class storage allocation: for each slot a component was just reduced into,
// pick the smallest class that fits its AABB, pop a block, and record (offset,
// dim) in the descriptor. World-fell bodies get the class for their extent; split
// children inherit the PARENT's frame dim (the split copies at parent-local
// coords), so they take a block fitting the parent's class but keep the parent's
// dim as their stride.
@group(0) @binding(0) var<storage, read> slotMeta : array<i32>;
@group(0) @binding(1) var<storage, read_write> desc : array<u32>;
@group(0) @binding(2) var<storage, read_write> classFree : array<atomic<u32>>;
@group(0) @binding(3) var<uniform> placeU : vec4<u32>; // x=isSplit, y=parent slot
@group(0) @binding(4) var<storage, read_write> occupied : array<u32>;

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
  let count = slotMeta[s * 16u + 6u];
  if (count <= 0) { return; } // no voxels at all here this op
  // Sub-threshold component: no body, no storage. Release the slot the register
  // pass claimed so it is reusable; the world-fell extract turns its voxels into
  // powder. Cull on the STRUCTURAL count ([14]) so a leaf-only clump (0 structural)
  // is released here, not leaked. WORLD FELL ONLY (placeU.x == 0): the body-SPLIT
  // extract has no cull path, so culling a split child here would leave it
  // allocated-but-unbacked and corrupt another body's block.
  if (placeU.x == 0u && slotMeta[s * 16u + 14u] < MIN_BODY_VOXELS) {
    occupied[s] = 0u;
    return;
  }
  let blk = popBlock();
  if (blk == 0xFFFFFFFFu) { return; } // pool full -> keep prior descriptor
  desc[s * 2u + 0u] = blk * BODYVOX;
  desc[s * 2u + 1u] = u32(BODYDIM);
}
