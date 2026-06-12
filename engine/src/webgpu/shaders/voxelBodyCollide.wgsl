// Tests each active body's solid voxels against the world at its current
// transform. For every voxel that penetrates world solid it accumulates a
// contact count + a sum of the contact world positions into that slot's 4-int
// record [count, sumX, sumY, sumZ], which the step pass turns into a contact
// centroid for its collision response. Brick-marched: one workgroup per body
// brick, empty bricks skipped.
struct Body {
  flags : vec4<u32>,
  invRot0 : vec4<f32>,
  invRot1 : vec4<f32>,
  invRot2 : vec4<f32>,
  center : vec4<f32>,
  pivot : vec4<f32>,
};
// Sparse brickmap body voxels (read-only).
@group(0) @binding(20) var<storage, read> bodyBrickGrid : array<u32>;
@group(0) @binding(21) var<storage, read> bodyBrickPool : array<u32>;
fn bodyVoxLoad(slot : u32, lx : i32, ly : i32, lz : i32) -> u32 {
  let bp = bodyBrickGrid[slot * BODYBRICKS + brickCell(lx, ly, lz)];
  if (bp == BRICK_EMPTY) { return 0u; }
  return bodyBrickPool[bp * 512u + brickLocal(lx, ly, lz)];
}
@group(0) @binding(1) var<storage, read> voxels : array<u32>;
@group(0) @binding(2) var<storage, read> bodies : array<Body, MAXB>;
@group(0) @binding(3) var<storage, read_write> contact : array<atomic<i32>>;
@group(0) @binding(4) var<storage, read> bricks : array<Brick>; // world brick occupancy

const SLOTVOX : u32 = BODYVOX;

fn vIndex(x : i32, y : i32, z : i32) -> u32 {
  let bi = (x / 8) + (y / 8) * BG + (z / 8) * BG * BG;
  return u32(bi) * 512u + u32((x % 8) + (y % 8) * 8 + (z % 8) * 64);
}
fn wSolid(x : i32, y : i32, z : i32) -> bool {
  if (y >= WG) { return false; } // open sky above
  // Out-of-bounds (horizontal edges + floor) reads as solid -- it must NOT index
  // the voxel buffer, where garbage flipped the contact normal sideways and flung
  // edge-bodies off the world (they then fell through). Walling the box also keeps
  // a body felled at the boundary resting instead of sliding out.
  if (x < 0 || x >= WG || z < 0 || z >= WG || y < 0) { return true; }
  return (voxels[vIndex(x, y, z)] & 3u) == 1u;
}
fn brickOcc(bx : i32, by : i32, bz : i32) -> u32 {
  if (bx < 0 || by < 0 || bz < 0 || bx >= BG || by >= BG || bz >= BG) { return 0u; }
  return bricks[u32(bx + by * BG + bz * BG * BG)].occupancy;
}

// Broad-phase: 1 if this brick's region is near any solid world this frame.
var<workgroup> wgNear : u32;
var<workgroup> wgBrick : u32; // brick pointer, broadcast from thread 0

// Brick-marched: one workgroup per body brick (12^3 grid), a z-column per thread.
// Empty bricks (most of a sparse body) cost one grid read + a uniform return, and
// the world-proximity broad-phase still skips bricks not near the ground.
@compute @workgroup_size(64)
fn collide(@builtin(workgroup_id) wid : vec3<u32>,
           @builtin(local_invocation_index) lidx : u32) {
  let slot = wid.z / u32(BODYBD);
  if (lidx == 0u) {
    wgNear = 0u;
    wgBrick = BRICK_EMPTY;
    if (slot < MAXB && bodies[slot].flags.x != 0u) {
      let bz = wid.z % u32(BODYBD);
      let cell = wid.x + wid.y * u32(BODYBD) + bz * u32(BODYBD) * u32(BODYBD);
      let bp = bodyBrickGrid[slot * BODYBRICKS + cell];
      wgBrick = bp;
      if (bp != BRICK_EMPTY) {
        let body = bodies[slot];
        let R = mat3x3<f32>(body.invRot0.xyz, body.invRot1.xyz, body.invRot2.xyz);
        let Rlw = transpose(R);
        let Lc = vec3<f32>(f32(wid.x * 8u) + 4.0,
                           f32(wid.y * 8u) + 4.0,
                           f32(bz * 8u) + 4.0);
        let wc = Rlw * (Lc - body.pivot.xyz) + body.center.xyz;
        let wb = vec3<i32>(floor(wc)) / 8;
        for (var dz = -1; dz <= 1; dz = dz + 1) {
          for (var dy = -1; dy <= 1; dy = dy + 1) {
            for (var dx = -1; dx <= 1; dx = dx + 1) {
              if (brickOcc(wb.x + dx, wb.y + dy, wb.z + dz) > 0u) { wgNear = 1u; }
            }
          }
        }
      }
    }
  }
  workgroupBarrier();
  if (wgBrick == BRICK_EMPTY || wgNear == 0u) { return; } // uniform skip

  let bp = wgBrick;
  let body = bodies[slot];
  let R = mat3x3<f32>(body.invRot0.xyz, body.invRot1.xyz, body.invRot2.xyz);
  let Rlw = transpose(R);
  let cx = i32(lidx % 8u); let cy = i32(lidx / 8u);
  let bmx = i32(wid.x) * 8; let bmy = i32(wid.y) * 8;
  let bmz = i32(wid.z % u32(BODYBD)) * 8;
  for (var cz = 0; cz < 8; cz = cz + 1) {
  let lx = bmx + cx; let ly = bmy + cy; let lz = bmz + cz;
  let v = bodyBrickPool[bp * 512u + u32(cx + cy * 8 + cz * 64)];
  if ((v & 3u) != 1u) { continue; }

  let L = vec3<f32>(f32(lx) + 0.5, f32(ly) + 0.5, f32(lz) + 0.5);
  let w = Rlw * (L - body.pivot.xyz) + body.center.xyz;
  let wi = vec3<i32>(floor(w));
  if (wi.x < 0 || wi.y < 0 || wi.z < 0 || wi.x >= WG || wi.y >= WG || wi.z >= WG) { continue; }
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
}
