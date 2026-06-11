// Returns a baked body's storage block to the free-list. The CPU lists the slots
// freed this frame (freeReq[0]=count, [1..]=slot ids); one thread per slot pushes
// its block id (offset / 96^3) back onto the class free-list.
@group(0) @binding(0) var<storage, read> desc : array<u32>;
@group(0) @binding(1) var<storage, read_write> classFree : array<atomic<u32>>;
@group(0) @binding(2) var<storage, read> freeReq : array<u32>;

@compute @workgroup_size(64)
fn freeStorage(@builtin(global_invocation_id) gid : vec3<u32>) {
  let tid = gid.x;
  if (tid >= freeReq[0]) { return; }
  let blk = desc[freeReq[tid + 1u] * 2u] / BODYVOX;
  let oldc = atomicAdd(&classFree[0], 1u);
  atomicStore(&classFree[oldc + 1u], blk);
}
