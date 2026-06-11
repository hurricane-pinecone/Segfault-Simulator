// Turns a freshly extracted slot's reduced metadata (AABB / CoM-sum / footprint-
// sum in slotMeta) into its initial motion state, one thread per slot. Replaces
// the CPU onSlotMetaMapped placement. A world-fell body sits at its world AABB
// and topples about its footprint; a split child renders in place in the parent
// frame and inherits the parent's fall -- and reads the parent's LIVE state here,
// so it is never a stale CPU capture. Slots with no component, or already-active
// slots (e.g. the split parent), are left untouched.
@group(0) @binding(0) var<storage, read> slotMeta : array<i32>;
@group(0) @binding(1) var<storage, read_write> state : array<vec4<f32>>;
@group(0) @binding(2) var<uniform> placeU : vec4<u32>; // x = isSplit, y = parent slot

@compute @workgroup_size(64)
fn placeBodies(@builtin(global_invocation_id) gid : vec3<u32>) {
  let s = gid.x;
  if (s >= MAXB) { return; }
  let base = s * 16u;
  let cnt = slotMeta[base + 6u];
  let voxN = slotMeta[base + 10u];
  let footN = slotMeta[base + 13u];
  if (cnt <= 0 || voxN <= 0 || footN <= 0) { return; } // no component this slot
  let flags = bitcast<u32>(state[s * 4u + 0u].x);
  if ((flags & 1u) != 0u) { return; } // live body (the split parent stays put)

  let footX = f32(slotMeta[base + 11u]) / f32(footN);
  let footZ = f32(slotMeta[base + 12u]) / f32(footN);
  var center : vec3<f32>;
  var pivot : vec3<f32>;
  var velY = 0.0;
  var wasActive = 0u;
  if (placeU.x != 0u) {
    // Split child: render in place (parent frame, theta 0), inherit the fall.
    let p = placeU.y;
    let pc = state[p * 4u + 1u].xyz;
    let pp = state[p * 4u + 2u].xyz;
    let minY = f32(slotMeta[base + 1u]);
    pivot = vec3<f32>(footX, minY, footZ);
    center = vec3<f32>(footX, minY, footZ) - pp + pc;
    velY = state[p * 4u + 0u].w;
    wasActive = 2u; // already seeded; do not let the step pass re-seed
  } else {
    // World fell: sit at the world AABB, topple about the ground footprint.
    center = vec3<f32>(f32(slotMeta[base + 0u]) + footX,
                       f32(slotMeta[base + 1u]),
                       f32(slotMeta[base + 2u]) + footZ);
    pivot = vec3<f32>(footX, 0.0, footZ);
  }
  // Topple axis perpendicular to the CoM lever (tips toward the heavy side).
  let inv = 1.0 / f32(voxN);
  let leverX = f32(slotMeta[base + 7u]) * inv - footX;
  let leverZ = f32(slotMeta[base + 9u]) * inv - footZ;
  let len = sqrt(leverX * leverX + leverZ * leverZ);
  var dx = 1.0;
  var dz = 0.0;
  if (len > 0.5) { dx = leverX / len; dz = leverZ / len; }

  state[s * 4u + 0u] = vec4<f32>(bitcast<f32>(1u | wasActive), 0.0, 0.0, velY);
  state[s * 4u + 1u] = vec4<f32>(center, 0.0);
  state[s * 4u + 2u] = vec4<f32>(pivot, 0.0);
  state[s * 4u + 3u] = vec4<f32>(dz, 0.0, -dx, 0.0);
}
