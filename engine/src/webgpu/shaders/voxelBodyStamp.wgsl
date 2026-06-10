// Stamps each resting body (flags.z set by the CPU) back into the world at its
// final pose, turning the fallen chunk into terrain again. Dispatched over MAXB
// stacked DIM^3 grids (slot = gid.z / DIM). Stamped at the rest pose, which is a
// clean axis swap, so it leaves no holes.
struct Body {
  flags : vec4<u32>,
  invRot0 : vec4<f32>,
  invRot1 : vec4<f32>,
  invRot2 : vec4<f32>,
  center : vec4<f32>,
  pivot : vec4<f32>,
};
@group(0) @binding(0) var<storage, read> bodyVox : array<u32>;
@group(0) @binding(1) var<storage, read_write> vox0 : array<u32>;
@group(0) @binding(2) var<storage, read_write> vox1 : array<u32>;
@group(0) @binding(3) var<storage, read> bodies : array<Body, MAXB>;

const SLOTVOX : u32 = 262144u;

fn vIndex(x : i32, y : i32, z : i32) -> u32 {
  let bi = (x / 8) + (y / 8) * BG + (z / 8) * BG * BG;
  return u32(bi) * 512u + u32((x % 8) + (y % 8) * 8 + (z % 8) * 64);
}

@compute @workgroup_size(4, 4, 4)
fn stamp(@builtin(global_invocation_id) gid : vec3<u32>) {
  let slot = gid.z / 64u;
  if (slot >= MAXB) { return; }
  let body = bodies[slot];
  if (body.flags.x == 0u || body.flags.z == 0u) { return; } // active + resting only
  let dim = i32(body.flags.y);
  let lx = i32(gid.x);
  let ly = i32(gid.y);
  let lz = i32(gid.z % 64u);
  if (lx >= dim || ly >= dim || lz >= dim) { return; }
  let v = bodyVox[slot * SLOTVOX + u32(lx + ly * dim + lz * dim * dim)];
  if ((v & 3u) != 1u) { return; }

  let R = mat3x3<f32>(body.invRot0.xyz, body.invRot1.xyz, body.invRot2.xyz);
  let Rlw = transpose(R);
  let L = vec3<f32>(f32(lx) + 0.5, f32(ly) + 0.5, f32(lz) + 0.5);
  let w = Rlw * (L - body.pivot.xyz) + body.center.xyz;
  let wi = vec3<i32>(floor(w));
  if (wi.x < 0 || wi.y < 0 || wi.z < 0 || wi.x >= WG || wi.y >= WG || wi.z >= WG) { return; }
  let vi = vIndex(wi.x, wi.y, wi.z);
  vox0[vi] = v;
  vox1[vi] = v;
}
