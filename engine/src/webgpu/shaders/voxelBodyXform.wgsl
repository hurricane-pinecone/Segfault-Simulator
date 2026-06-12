// Builds each slot's transform uniform from its motion state, one thread per
// slot: the orientation quaternion -> rotation matrix, packed to match the Body
// layout the collide/stamp/edit/render passes read (6 vec4/slot -- [0] flags
// (active,dim,bakeReady,carveable), [1..3] rotation rows, [4] center, [5] CoM).
// State stride is 8 vec4: [0]=(flags,timer,_,_) [1]=center [3]=quat [5]=com.
@group(0) @binding(0) var<storage, read> state : array<vec4<f32>>;
@group(0) @binding(1) var<storage, read_write> xform : array<vec4<f32>>;
// Sparse brickmap grid (read): scan the slot's occupied bricks for a tight local
// AABB, packed into the spare invRot .w lanes for the render's debug box.
@group(0) @binding(20) var<storage, read> bodyBrickGrid : array<u32>;

// Pack a 0..BODYDIM local coord triple into one u32 (8 bits/axis; 96 < 256).
fn packAABB(v : vec3<i32>) -> u32 {
  let c = clamp(v, vec3<i32>(0), vec3<i32>(BODYDIM));
  return u32(c.x) | (u32(c.y) << 8u) | (u32(c.z) << 16u);
}

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

  // Tight local AABB of the occupied bricks (brick-granular, so a multiple of 8),
  // which is the extent the render actually marches voxels in -- the debug box
  // frames this rather than the full BODYDIM^3 grid. Degenerate (lo > hi) when the
  // body has no bricks; the render skips the box then.
  var lo = vec3<i32>(BODYDIM);
  var hi = vec3<i32>(0);
  if (act != 0u) {
    let g0 = s * BODYBRICKS;
    for (var i = 0u; i < BODYBRICKS; i = i + 1u) {
      if (bodyBrickGrid[g0 + i] != BRICK_EMPTY) {
        let bx = i32(i % u32(BODYBD));
        let by = i32((i / u32(BODYBD)) % u32(BODYBD));
        let bz = i32(i / u32(BODYBD * BODYBD));
        let p = vec3<i32>(bx, by, bz) * 8;
        lo = min(lo, p);
        hi = max(hi, p + 8);
      }
    }
  }

  let base = s * 6u;
  // Every body grid is BODYDIM^3 (the sparse brickmap is the storage), so the dim
  // is constant -- no per-slot descriptor.
  xform[base + 0u] = vec4<f32>(bitcast<f32>(act),
                               bitcast<f32>(u32(BODYDIM)),
                               bitcast<f32>(bake),
                               bitcast<f32>(act));
  xform[base + 1u] = vec4<f32>(m00, m01, m02, bitcast<f32>(packAABB(lo)));
  xform[base + 2u] = vec4<f32>(m10, m11, m12, bitcast<f32>(packAABB(hi)));
  xform[base + 3u] = vec4<f32>(m20, m21, m22, 0.0);
  xform[base + 4u] = vec4<f32>(center, 0.0);
  xform[base + 5u] = vec4<f32>(com, 0.0);
}
