// Tests the rigid body's solid voxels against the world at the body's current
// transform; sets a flag if any of them overlap world solid. The CPU uses this
// to stop a straight-down fall the moment the body lands, then start the topple.
struct Body {
  flags : vec4<u32>,
  invRot0 : vec4<f32>,
  invRot1 : vec4<f32>,
  invRot2 : vec4<f32>,
  center : vec4<f32>,
  pivot : vec4<f32>,
};
@group(0) @binding(0) var<storage, read> bodyVox : array<u32>;
@group(0) @binding(1) var<storage, read> voxels : array<u32>;
@group(0) @binding(2) var<uniform> body : Body;
@group(0) @binding(3) var<storage, read_write> collideFlag : array<atomic<u32>>;

fn vIndex(x : i32, y : i32, z : i32) -> u32 {
  let bi = (x / 8) + (y / 8) * BG + (z / 8) * BG * BG;
  return u32(bi) * 512u + u32((x % 8) + (y % 8) * 8 + (z % 8) * 64);
}

@compute @workgroup_size(4, 4, 4)
fn collide(@builtin(global_invocation_id) gid : vec3<u32>) {
  let dim = i32(body.flags.y);
  let lx = i32(gid.x);
  let ly = i32(gid.y);
  let lz = i32(gid.z);
  if (lx >= dim || ly >= dim || lz >= dim) { return; }
  let v = bodyVox[u32(lx + ly * dim + lz * dim * dim)];
  if ((v & 3u) != 1u) { return; } // solid body voxels only

  // body-local -> world: local-to-world rotation is the transpose of invRot.
  let R = mat3x3<f32>(body.invRot0.xyz, body.invRot1.xyz, body.invRot2.xyz);
  let Rlw = transpose(R);
  let L = vec3<f32>(f32(lx) + 0.5, f32(ly) + 0.5, f32(lz) + 0.5);
  let w = Rlw * (L - body.pivot.xyz) + body.center.xyz;
  let wi = vec3<i32>(floor(w));
  if (wi.x < 0 || wi.y < 0 || wi.z < 0 || wi.x >= WG || wi.y >= WG || wi.z >= WG) { return; }
  if ((voxels[vIndex(wi.x, wi.y, wi.z)] & 3u) == 1u) { atomicOr(&collideFlag[0], 1u); }
}
