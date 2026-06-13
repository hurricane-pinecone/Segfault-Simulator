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
  // Skip empty slots and sub-threshold fragments (culled to powder/dropped by the
  // extract + placeStorage, so they must not be activated as bodies here).
  if (slotMeta[base + 6u] < MIN_BODY_VOXELS || voxN <= 0) { return; }
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

  // Density-weighted scalar moment of inertia = mass-weighted mean squared
  // distance from the CoM (radius of gyration^2): variance E[r^2] - E[r]^2 per
  // axis, summed. Light voxels (leaves) barely add to rotational resistance, just
  // as they barely shift the CoM. Stored in the CoM slot's .w (the step reads it;
  // the xform reads only .xyz).
  let e2 = vec3<f32>(f32(slotMeta[base + 11u]),
                     f32(slotMeta[base + 12u]),
                     f32(slotMeta[base + 13u])) * inv * 16.0; // undo the /16 scale
  let varv = e2 - com * com;
  let inertia = max(varv.x + varv.y + varv.z, 4.0);

  state[s * 8u + 0u] = vec4<f32>(bitcast<f32>(1u), 0.0, 0.0, 0.0); // active, timer 0
  state[s * 8u + 1u] = vec4<f32>(center, 0.0);
  state[s * 8u + 2u] = vec4<f32>(linVel, 0.0);
  state[s * 8u + 3u] = quat;
  state[s * 8u + 4u] = vec4<f32>(angVel, 0.0);
  state[s * 8u + 5u] = vec4<f32>(com, inertia);
  // Clear the shed/break-off request (slot 6): placed AFTER this frame's step ran,
  // so without resetting it the new body inherits the previous tenant's leftover
  // impact and the shed pass tears voxels off it the moment it spawns.
  state[s * 8u + 6u] = vec4<f32>(0.0);
  // Slot 7.x = mass (density-weighted voxel count), for mass-scaled impulses (a
  // blast displaces a heavy wood-filled chunk less than light shrapnel).
  state[s * 8u + 7u] = vec4<f32>(f32(voxN), 0.0, 0.0, 0.0);
}
