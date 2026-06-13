// Blast impulse: one thread per rigid-body slot. Pushes every active body within
// the blast's reach outward from the crater (with an up-bias + a random tumble),
// scaled by a linear falloff. Runs after the fell's place, so it flings BOTH the
// freshly detached chunks and any pre-existing bodies in range -- one pass for
// both. The step pass then integrates the new velocity under gravity.
@group(0) @binding(0) var<storage, read_write> state : array<vec4<f32>>;
@group(0) @binding(1) var<storage, read> blast : array<f32>; // [0..2]center [3]r [4]force [5]active

@compute @workgroup_size(64)
fn impulse(@builtin(global_invocation_id) gid : vec3<u32>) {
  let s = gid.x;
  if (s >= MAXB) { return; }
  if (blast[5] == 0.0) { return; } // ray missed -> no blast this frame
  let base = s * 8u;
  let flags = bitcast<u32>(state[base + 0u].x);
  if ((flags & 1u) == 0u) { return; } // inactive slot
  if ((flags & 2u) != 0u) { return; } // bake-ready: frozen, leave it

  let bc = state[base + 1u].xyz; // body world CoM
  let blastC = vec3<f32>(blast[0], blast[1], blast[2]);
  let force = blast[4];
  let reach = blast[3] * 5.0; // the shockwave carries well past the crater rim
  let d = bc - blastC;
  let dist = length(d);
  if (dist > reach) { return; }
  let dir = select(d / max(dist, 0.001), vec3<f32>(0.0, 1.0, 0.0), dist < 0.001);
  // Mass-scaled impulse: dv = J / mass. A chunk at/under MASS_REF gets the full
  // kick; a heavier one is displaced proportionally less, so a wood-filled body
  // barely shifts where light shrapnel flies.
  let mass = state[base + 7u].x;
  let MASS_REF = 800.0; // chunks at/under this fly full; heavier scale ~ 1/mass
  let mscale = MASS_REF / max(mass, MASS_REF);
  let mag = force * (1.0 - dist / reach) * mscale; // linear falloff, mass-scaled

  var lin = state[base + 2u].xyz;
  lin = lin + dir * mag + vec3<f32>(0.0, mag * 0.5, 0.0); // outward + up
  state[base + 2u] = vec4<f32>(lin, 0.0);

  // Per-body random tumble so chunks spin as they fly.
  let h = hash3(s, u32(bc.x), u32(bc.z), 1u);
  let spin = (vec3<f32>(f32(h & 0xFFu),
                        f32((h >> 8u) & 0xFFu),
                        f32((h >> 16u) & 0xFFu)) / 255.0 - 0.5);
  var ang = state[base + 4u].xyz;
  ang = ang + spin * mag * 0.12;
  state[base + 4u] = vec4<f32>(ang, 0.0);
}
