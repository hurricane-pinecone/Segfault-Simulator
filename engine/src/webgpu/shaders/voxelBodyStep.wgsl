// Per-body rigid motion, one thread per pool slot. Replaces the CPU stepBody:
// a freshly placed body is seeded, then falls straight down under gravity until
// the body-vs-world collide pass flags an overlap, then topples (inverted
// pendulum about its footprint) until flat, where it rests (stamp pending). State
// is 4 vec4 per slot: [0]=(flags,theta,omega,velY), [1]=center, [2]=pivot,
// [3]=axis. flags bits: 0 active, 1 wasActive (seeded), 2 landed, 3 resting.
@group(0) @binding(0) var<storage, read_write> state : array<vec4<f32>>;
@group(0) @binding(1) var<storage, read> collide : array<u32>;
@group(0) @binding(2) var<uniform> stepU : vec4<f32>; // x = dt

const HALF_PI : f32 = 1.5707963;

@compute @workgroup_size(64)
fn stepBodies(@builtin(global_invocation_id) gid : vec3<u32>) {
  let s = gid.x;
  if (s >= MAXB) { return; }
  var s0 = state[s * 4u + 0u];
  var flags = bitcast<u32>(s0.x);
  if ((flags & 1u) == 0u) { return; } // inactive slot
  let dt = stepU.x;
  var theta = s0.y;
  var omega = s0.z;
  var velY = s0.w;
  var c = state[s * 4u + 1u];
  var landed = (flags & 4u) != 0u;
  var resting = (flags & 8u) != 0u;
  if ((flags & 2u) == 0u) {
    // First tick: seed and stop. Integration waits until next tick, when this
    // body's collide flag has actually been tested (the buffer is otherwise stale
    // for a freshly placed slot, which could false-trigger a mid-air landing).
    flags = (flags | 2u) & ~12u;
    state[s * 4u + 0u] = vec4<f32>(bitcast<f32>(flags), 0.0, 0.0, 0.0);
    return;
  }
  if (!resting) {
    if (!landed) {
      if (collide[s] != 0u) { landed = true; omega = 0.4; } // ground contact
      else { velY = velY - 80.0 * dt; c.y = c.y + velY * dt; }
    } else if (theta < HALF_PI) {
      omega = omega + 2.5 * sin(theta) * dt;
      theta = theta + omega * dt;
      if (theta > HALF_PI) { theta = HALF_PI; resting = true; }
    }
  }
  flags = (flags & ~12u) | select(0u, 4u, landed) | select(0u, 8u, resting);
  state[s * 4u + 0u] = vec4<f32>(bitcast<f32>(flags), theta, omega, velY);
  state[s * 4u + 1u] = c;
}
