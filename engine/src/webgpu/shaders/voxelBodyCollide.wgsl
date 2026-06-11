// Tests each active body's solid voxels against the world at its current
// transform. For every voxel that penetrates world solid it accumulates a
// contact count + a sum of the contact world positions into that slot's 4-int
// record [count, sumX, sumY, sumZ], which the step pass turns into a contact
// centroid for its collision response. Dispatched over MAXB stacked DIM^3 grids.
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
@group(0) @binding(2) var<storage, read> bodies : array<Body, MAXB>;
@group(0) @binding(3) var<storage, read_write> contact : array<atomic<i32>>;

const SLOTVOX : u32 = BODYVOX;

fn vIndex(x : i32, y : i32, z : i32) -> u32 {
  let bi = (x / 8) + (y / 8) * BG + (z / 8) * BG * BG;
  return u32(bi) * 512u + u32((x % 8) + (y % 8) * 8 + (z % 8) * 64);
}
fn wSolid(x : i32, y : i32, z : i32) -> bool {
  if (y >= WG) { return false; }
  return (voxels[vIndex(x, y, z)] & 3u) == 1u;
}

@compute @workgroup_size(4, 4, 4)
fn collide(@builtin(global_invocation_id) gid : vec3<u32>) {
  let slot = gid.z / u32(BODYDIM);
  if (slot >= MAXB) { return; }
  let body = bodies[slot];
  if (body.flags.x == 0u) { return; }
  let dim = i32(body.flags.y);
  let lx = i32(gid.x);
  let ly = i32(gid.y);
  let lz = i32(gid.z % u32(BODYDIM));
  if (lx >= dim || ly >= dim || lz >= dim) { return; }
  let v = bodyVox[slot * SLOTVOX + u32(lx + ly * dim + lz * dim * dim)];
  if ((v & 3u) != 1u) { return; }

  let R = mat3x3<f32>(body.invRot0.xyz, body.invRot1.xyz, body.invRot2.xyz);
  let Rlw = transpose(R);
  let L = vec3<f32>(f32(lx) + 0.5, f32(ly) + 0.5, f32(lz) + 0.5);
  let w = Rlw * (L - body.pivot.xyz) + body.center.xyz;
  let wi = vec3<i32>(floor(w));
  if (wi.x < 0 || wi.y < 0 || wi.z < 0 || wi.x >= WG || wi.y >= WG || wi.z >= WG) { return; }
  if (wSolid(wi.x, wi.y, wi.z)) {
    // Penetration depth = solid voxels from here up to the surface (capped).
    var pen = 1;
    for (var k = 1; k <= 16; k = k + 1) {
      if (!wSolid(wi.x, wi.y + k, wi.z)) { break; }
      pen = k + 1;
    }
    // Surface normal estimate: directions to neighbouring air point outward, so
    // a flat top gives +y and a slope gives a tilted normal. Summed per body.
    var nrm = vec3<i32>(0, 0, 0);
    if (!wSolid(wi.x + 1, wi.y, wi.z)) { nrm.x = nrm.x + 1; }
    if (!wSolid(wi.x - 1, wi.y, wi.z)) { nrm.x = nrm.x - 1; }
    if (!wSolid(wi.x, wi.y + 1, wi.z)) { nrm.y = nrm.y + 1; }
    if (!wSolid(wi.x, wi.y - 1, wi.z)) { nrm.y = nrm.y - 1; }
    if (!wSolid(wi.x, wi.y, wi.z + 1)) { nrm.z = nrm.z + 1; }
    if (!wSolid(wi.x, wi.y, wi.z - 1)) { nrm.z = nrm.z - 1; }
    let b = i32(slot) * 8;
    atomicAdd(&contact[b + 0], 1);
    atomicAdd(&contact[b + 1], wi.x);
    atomicAdd(&contact[b + 2], wi.y);
    atomicAdd(&contact[b + 3], wi.z);
    atomicMax(&contact[b + 4], pen);
    atomicAdd(&contact[b + 5], nrm.x);
    atomicAdd(&contact[b + 6], nrm.y);
    atomicAdd(&contact[b + 7], nrm.z);
  }
}
