// Builds each slot's transform uniform from its motion state, one thread per
// slot. The Rodrigues rotation (world->local, transpose applied by consumers)
// about the topple axis, packed to match the Body layout the collide/stamp/edit/
// render passes read: 6 vec4 per slot -- [0] flags(active,dim,resting,carveable),
// [1..3] rotation rows, [4] center, [5] pivot.
@group(0) @binding(0) var<storage, read> state : array<vec4<f32>>;
@group(0) @binding(1) var<storage, read_write> xform : array<vec4<f32>>;

@compute @workgroup_size(64)
fn buildXform(@builtin(global_invocation_id) gid : vec3<u32>) {
  let s = gid.x;
  if (s >= MAXB) { return; }
  let s0 = state[s * 4u + 0u];
  let flags = bitcast<u32>(s0.x);
  let theta = s0.y;
  let act = flags & 1u; // active is a WGSL reserved word
  let rest = select(0u, 1u, (flags & 8u) != 0u);
  let center = state[s * 4u + 1u].xyz;
  let pivot = state[s * 4u + 2u].xyz;
  let a = state[s * 4u + 3u].xyz;
  let c = cos(theta);
  let sn = sin(theta);
  let t = 1.0 - c;
  let base = s * 6u;
  xform[base + 0u] = vec4<f32>(bitcast<f32>(act),
                               bitcast<f32>(u32(BODYDIM)),
                               bitcast<f32>(rest),
                               bitcast<f32>(act));
  xform[base + 1u] = vec4<f32>(t * a.x * a.x + c,
                               t * a.x * a.y - sn * a.z,
                               t * a.x * a.z + sn * a.y, 0.0);
  xform[base + 2u] = vec4<f32>(t * a.x * a.y + sn * a.z,
                               t * a.y * a.y + c,
                               t * a.y * a.z - sn * a.x, 0.0);
  xform[base + 3u] = vec4<f32>(t * a.x * a.z - sn * a.y,
                               t * a.y * a.z + sn * a.x,
                               t * a.z * a.z + c, 0.0);
  xform[base + 4u] = vec4<f32>(center, 0.0);
  xform[base + 5u] = vec4<f32>(pivot, 0.0);
}
