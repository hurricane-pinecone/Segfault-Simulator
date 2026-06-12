// GPU-driven sizing for the per-frame body passes. Scans the rigid-body state
// flags and finds activeHigh = highest active slot + 1, then writes:
//   - the indirect dispatch args (24, 24, 24*activeHigh) consumed by
//     collide/shed/stamp via dispatchWorkgroupsIndirect, and
//   - dbg[0].z = activeHigh, the render's per-pixel body-loop bound.
// This replaces the CPU mirror sizing those passes from a 1-frame-stale count
// (the lag-vs-vanishing bug): the GPU sizes its own work from current truth.
@group(0) @binding(0) var<storage, read> state : array<vec4<f32>>;
@group(0) @binding(1) var<storage, read_write> args : array<u32>;
@group(0) @binding(2) var<storage, read_write> dbg : array<vec4<f32>>;

var<workgroup> wgHigh : atomic<u32>;

@compute @workgroup_size(64)
fn prep(@builtin(local_invocation_id) lid : vec3<u32>) {
  if (lid.x == 0u) { atomicStore(&wgHigh, 0u); }
  workgroupBarrier();
  for (var s = lid.x; s < MAXB; s = s + 64u) {
    let flags = bitcast<u32>(state[s * 8u + 0u].x);
    if ((flags & 1u) != 0u) { atomicMax(&wgHigh, s + 1u); }
  }
  workgroupBarrier();
  if (lid.x == 0u) {
    let hi = atomicLoad(&wgHigh);
    let g = u32(BODYDIM) / 4u;
    args[0] = g;
    args[1] = g;
    args[2] = g * hi;
    args[3] = hi; // render reads its body-loop bound from here (storage, reliable)
    // Brick dispatch (one workgroup per body brick) for the brick-marched passes.
    let bd = u32(BODYBD);
    args[4] = bd;
    args[5] = bd;
    args[6] = bd * hi;
    dbg[0].z = f32(hi);
  }
}
