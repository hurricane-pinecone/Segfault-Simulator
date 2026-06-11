// Builds each slot's transform uniform from its motion state, one thread per
// slot: the orientation quaternion -> rotation matrix, packed to match the Body
// layout the collide/stamp/edit/render passes read (6 vec4/slot -- [0] flags
// (active,dim,bakeReady,carveable), [1..3] rotation rows, [4] center, [5] CoM).
// State stride is 8 vec4: [0]=(flags,timer,_,_) [1]=center [3]=quat [5]=com.
@group(0) @binding(0) var<storage, read> state : array<vec4<f32>>;
@group(0) @binding(1) var<storage, read_write> xform : array<vec4<f32>>;
@group(0) @binding(2) var<storage, read> desc : array<u32>; // per-slot (offset, dim)

@compute @workgroup_size(64)
fn buildXform(@builtin(global_invocation_id) gid : vec3<u32>) {
  let s = gid.x;
  if (s >= MAXB) { return; }
  let sbase = s * 8u;
  let flags = bitcast<u32>(state[sbase + 0u].x);
  let act = flags & 1u;
  let bake = select(0u, 1u, (flags & 2u) != 0u);
  let center = state[sbase + 1u].xyz;
  let q = state[sbase + 3u];
  let com = state[sbase + 5u].xyz;

  let x = q.x; let y = q.y; let z = q.z; let w = q.w;
  let m00 = 1.0 - 2.0 * (y * y + z * z);
  let m01 = 2.0 * (x * y - w * z);
  let m02 = 2.0 * (x * z + w * y);
  let m10 = 2.0 * (x * y + w * z);
  let m11 = 1.0 - 2.0 * (x * x + z * z);
  let m12 = 2.0 * (y * z - w * x);
  let m20 = 2.0 * (x * z - w * y);
  let m21 = 2.0 * (y * z + w * x);
  let m22 = 1.0 - 2.0 * (x * x + y * y);

  let base = s * 6u;
  let off = desc[s * 2u + 0u];      // body's base offset into the voxel pool
  let dm = desc[s * 2u + 1u];       // body's grid dim (size class)
  xform[base + 0u] = vec4<f32>(bitcast<f32>(act),
                               bitcast<f32>(dm),
                               bitcast<f32>(bake),
                               bitcast<f32>(act));
  xform[base + 1u] = vec4<f32>(m00, m01, m02, 0.0);
  xform[base + 2u] = vec4<f32>(m10, m11, m12, 0.0);
  xform[base + 3u] = vec4<f32>(m20, m21, m22, 0.0);
  xform[base + 4u] = vec4<f32>(center, 0.0);
  xform[base + 5u] = vec4<f32>(com, bitcast<f32>(off)); // pivot.w = pool offset
}
