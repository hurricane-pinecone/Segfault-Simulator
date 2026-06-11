// Initialises a freshly extracted slot's rigid-body state from its reduced
// metadata (AABB / CoM-sum / count in slotMeta), one thread per slot. The body
// rotates about its centre of mass: com = CoM-sum/count (body-local), and the
// world CoM sits at aabbMin + com. A world-fell body starts at rest, upright; a
// split child inherits the parent's LIVE pose + velocities (read here, never a
// stale CPU capture) so it continues the parent's motion seamlessly.
// State stride 8 vec4: [0]=(flags,timer,_,_) [1]=center [2]=linVel [3]=quat
// [4]=angVel [5]=com.
@group(0) @binding(0) var<storage, read> slotMeta : array<i32>;
@group(0) @binding(1) var<storage, read_write> state : array<vec4<f32>>;
@group(0) @binding(2) var<uniform> placeU : vec4<u32>; // x = isSplit, y = parent slot

// local-frame rotation of a vector by quaternion q (q (x) v (x) q*).
fn qrot(q : vec4<f32>, v : vec3<f32>) -> vec3<f32> {
  let u = vec3<f32>(q.x, q.y, q.z);
  return v + 2.0 * cross(u, cross(u, v) + q.w * v);
}

@compute @workgroup_size(64)
fn placeBodies(@builtin(global_invocation_id) gid : vec3<u32>) {
  let s = gid.x;
  if (s >= MAXB) { return; }
  let base = s * 16u;
  let voxN = slotMeta[base + 10u];
  if (slotMeta[base + 6u] <= 0 || voxN <= 0) { return; } // no component this slot
  let flags = bitcast<u32>(state[s * 8u + 0u].x);
  if ((flags & 1u) != 0u) { return; } // live body (the split parent stays put)

  let inv = 1.0 / f32(voxN);
  let com = vec3<f32>(f32(slotMeta[base + 7u]),
                      f32(slotMeta[base + 8u]),
                      f32(slotMeta[base + 9u])) * inv;
  var center : vec3<f32>;
  var quat = vec4<f32>(0.0, 0.0, 0.0, 1.0);
  var linVel = vec3<f32>(0.0);
  var angVel = vec3<f32>(0.0);
  if (placeU.x != 0u) {
    // Split child: ride the parent's pose + velocities.
    let p = placeU.y * 8u;
    quat = state[p + 3u];
    linVel = state[p + 2u].xyz;
    angVel = state[p + 4u].xyz;
    let pc = state[p + 1u].xyz;  // parent world CoM
    let pcom = state[p + 5u].xyz; // parent body-local CoM
    center = qrot(quat, com - pcom) + pc;
  } else {
    // World fell: world CoM = AABB origin + body-local CoM, at rest, upright.
    center = vec3<f32>(f32(slotMeta[base + 0u]),
                       f32(slotMeta[base + 1u]),
                       f32(slotMeta[base + 2u])) + com;
  }

  // Scalar moment of inertia from the body extent (radius-of-gyration^2, unit
  // mass): a big chunk resists spinning, a pebble tumbles freely. Stored in the
  // CoM slot's .w (the step reads it; the xform only reads .xyz).
  let ext = vec3<f32>(f32(slotMeta[base + 3u] - slotMeta[base + 0u]),
                      f32(slotMeta[base + 4u] - slotMeta[base + 1u]),
                      f32(slotMeta[base + 5u] - slotMeta[base + 2u]));
  let inertia = max(dot(ext, ext) / 12.0, 4.0);

  state[s * 8u + 0u] = vec4<f32>(bitcast<f32>(1u), 0.0, 0.0, 0.0); // active, timer 0
  state[s * 8u + 1u] = vec4<f32>(center, 0.0);
  state[s * 8u + 2u] = vec4<f32>(linVel, 0.0);
  state[s * 8u + 3u] = quat;
  state[s * 8u + 4u] = vec4<f32>(angVel, 0.0);
  state[s * 8u + 5u] = vec4<f32>(com, inertia);
}
