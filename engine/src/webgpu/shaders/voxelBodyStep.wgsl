// Rigid-body integration, one thread per pool slot. Full 6-DOF: gravity on the
// linear velocity, a quaternion orientation advanced by the angular velocity,
// and a contact response from the collide pass (penetration count + centroid)
// that pushes the body out of the ground, kills into-surface velocity, applies
// friction, and torques the body about the contact so off-centre / sloped
// landings tip and roll. A settle timer ticks only while grounded and nearly
// still (and resets the instant it moves), so a chunk can roll downhill and only
// bakes into terrain once it has truly stopped.
//
// State = 8 vec4 / slot: [0]=(flags,settleTimer,_,_) [1]=center(world CoM)
// [2]=linVel [3]=quat(xyzw) [4]=angVel [5]=com(body-local CoM). flags: bit0
// active, bit1 bakeReady.
@group(0) @binding(0) var<storage, read_write> state : array<vec4<f32>>;
@group(0) @binding(1) var<storage, read> contact : array<i32>; // [0]count [1..3]posSum
@group(0) @binding(2) var<uniform> stepU : vec4<f32>; // x = dt

const G : f32 = 80.0;          // gravity
const REST_TIME : f32 = 0.6;   // grounded+still seconds before baking to terrain
const V_REST : f32 = 0.7;      // horizontal + angular "still" threshold
const RESTITUTION : f32 = 0.0; // bounciness (0 = inelastic)
const FRICTION : f32 = 0.5;    // Coulomb friction coefficient
const PUSH : f32 = 0.5;        // penetration-resolve gain (fraction of depth/frame)
const MAX_W : f32 = 2.2;       // angular speed cap (keeps thin parts from tunnelling)
const ROLL_DAMP : f32 = 0.97;  // per-grounded-frame angular damping (light, so it rolls downhill)
const LIN_DAMP : f32 = 0.98;   // per-grounded-frame horizontal damping (light)
const SHED_THRESHOLD : f32 = 8.0; // normal impulse above which voxels break off

@compute @workgroup_size(64)
fn stepBodies(@builtin(global_invocation_id) gid : vec3<u32>) {
  let s = gid.x;
  if (s >= MAXB) { return; }
  let base = s * 8u;
  var s0 = state[base + 0u];
  var flags = bitcast<u32>(s0.x);
  if ((flags & 1u) == 0u) { return; }   // inactive slot
  if ((flags & 2u) != 0u) { return; }   // bake-ready: frozen, waiting for the stamp
  let dt = stepU.x;
  var timer = s0.y;
  var center = state[base + 1u].xyz;
  var linVel = state[base + 2u].xyz;
  var q = state[base + 3u];
  var angVel = state[base + 4u].xyz;
  let inertia = state[base + 5u].w; // per-body moment of inertia (size-scaled)

  linVel.y = linVel.y - G * dt;

  // Contact response: a single aggregated contact at the centroid of penetrating
  // voxels, ground normal up. Resolve as an IMPULSE on the velocity at the contact
  // (linear + angular), which dissipates energy and settles -- a continuous torque
  // pumps energy in and spins forever. Friction is Coulomb-clamped.
  let cnt = contact[s * 8 + 0];
  let grounded = cnt > 0;
  var shedImpact = 0.0;             // hard-impact magnitude for the break-off pass
  var shedC = vec3<f32>(0.0);       // contact point of that impact
  if (grounded) {
    let inv = 1.0 / f32(cnt);
    let p = vec3<f32>(f32(contact[s * 8 + 1]),
                      f32(contact[s * 8 + 2]),
                      f32(contact[s * 8 + 3])) * inv;
    // Contact normal from the terrain around the contact (slopes tilt it); fall
    // back to up if degenerate.
    let nsum = vec3<f32>(f32(contact[s * 8 + 5]),
                         f32(contact[s * 8 + 6]),
                         f32(contact[s * 8 + 7]));
    var n = vec3<f32>(0.0, 1.0, 0.0);
    if (dot(nsum, nsum) > 0.0001) { n = normalize(nsum); }
    let r = p - center;                       // contact relative to the CoM
    let vp = linVel + cross(angVel, r);       // world velocity at the contact
    let vn = dot(vp, n);
    if (vn < 0.0) {                           // moving into the surface
      let rxn = cross(r, n);
      let jn = -(1.0 + RESTITUTION) * vn / (1.0 + dot(rxn, rxn) / inertia);
      linVel = linVel + jn * n;
      angVel = angVel + cross(r, jn * n) / inertia;
      if (jn > SHED_THRESHOLD) { shedImpact = jn; shedC = p; } // hard hit -> break off
      // Friction opposes the tangential velocity, clamped to the friction cone.
      let vt = vp - vn * n;
      let vtl = length(vt);
      if (vtl > 0.001) {
        let t = vt / vtl;
        let rxt = cross(r, t);
        let jt = max(-vtl / (1.0 + dot(rxt, rxt) / inertia), -FRICTION * jn);
        linVel = linVel + jt * t;
        angVel = angVel + cross(r, jt * t) / inertia;
      }
    }
    // Rolling + sliding resistance so a round chunk (a canopy) doesn't roll
    // forever and pieces stop drifting -- this is most of the "has weight" feel.
    angVel = angVel * ROLL_DAMP;
    linVel.x = linVel.x * LIN_DAMP;
    linVel.z = linVel.z * LIN_DAMP;
    // Resolve penetration along the contact normal by the measured depth,
    // leaving ~1 voxel of rest overlap so the push goes to zero at rest.
    let pen = f32(contact[s * 8 + 4]);
    center = center + n * max(0.0, pen - 1.0) * PUSH;
  }

  let wlen = length(angVel);
  if (wlen > MAX_W) { angVel = angVel * (MAX_W / wlen); }

  center = center + linVel * dt;

  // Quaternion integration: q += 0.5 * (omega (x) q) * dt, renormalised.
  let wx = angVel.x; let wy = angVel.y; let wz = angVel.z;
  let dq = vec4<f32>(wx * q.w + wy * q.z - wz * q.y,
                     -wx * q.z + wy * q.w + wz * q.x,
                     wx * q.y - wy * q.x + wz * q.w,
                     -(wx * q.x + wy * q.y + wz * q.z));
  q = normalize(q + 0.5 * dt * dq);

  // Settle: only count time while grounded and barely moving (vertical bob from
  // the contact push is ignored, or it would never settle). Any real motion
  // resets it, so a body can roll a long way before it bakes.
  let still = length(linVel.xz) + length(angVel) * 8.0;
  if (grounded && still < V_REST) {
    timer = timer + dt;
  } else {
    timer = 0.0;
  }
  if (timer > REST_TIME) { flags = flags | 2u; }

  state[base + 0u] = vec4<f32>(bitcast<f32>(flags), timer, 0.0, 0.0);
  state[base + 1u] = vec4<f32>(center, 0.0);
  state[base + 2u] = vec4<f32>(linVel, 0.0);
  state[base + 3u] = q;
  state[base + 4u] = vec4<f32>(angVel, 0.0);
  state[base + 6u] = vec4<f32>(shedImpact, shedC.x, shedC.y, shedC.z); // break-off request
}
