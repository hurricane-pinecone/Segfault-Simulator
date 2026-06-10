// Click edit: ray-march to the nearest solid hit across the world AND the active
// rigid bodies, then carve (mode 1) or spawn water (mode 2) a sphere there. World
// hits carve both world buffers + mark bricks dirty; body hits carve that body's
// local grid (single-buffered, no brickmap). One workgroup: thread 0 marches,
// all carve. Only upright (flags.w) bodies are carveable -- a toppling body's
// world-down is rotated in its grid, so the footprint logic would not hold.
struct Edit { v0 : vec4<f32>, v1 : vec4<f32> }; // origin.xyz,radius ; dir.xyz,mode
struct Body {
  flags : vec4<u32>,   // x active, y dim, z resting, w carveable (upright/falling)
  invRot0 : vec4<f32>,
  invRot1 : vec4<f32>,
  invRot2 : vec4<f32>,
  center : vec4<f32>,
  pivot : vec4<f32>,
};
@group(0) @binding(0) var<storage, read_write> voxCur : array<u32>;
@group(0) @binding(1) var<storage, read_write> voxOther : array<u32>;
@group(0) @binding(2) var<storage, read> bricks : array<Brick>;
@group(0) @binding(3) var<uniform> ed : Edit;
@group(0) @binding(4) var<storage, read_write> dirty : array<atomic<u32>>;
@group(0) @binding(5) var<storage, read_write> bodyVox : array<u32>;
@group(0) @binding(6) var<storage, read> bodies : array<Body, MAXB>;
@group(0) @binding(7) var<storage, read_write> carveHit : array<u32>; // [0]body? [1]slot [2..4]hit

const SLOTVOX : u32 = 262144u; // body grid DIM^3

fn vIdx(x : i32, y : i32, z : i32) -> u32 {
  let bi = u32((x / 8) + (y / 8) * BG + (z / 8) * BG * BG);
  return bricks[bi].pointer * 512u + u32((x % 8) + (y % 8) * 8 + (z % 8) * 64);
}
fn inW(x : i32, y : i32, z : i32) -> bool { return x >= 0 && y >= 0 && z >= 0 && x < WG && y < WG && z < WG; }

struct March { found : bool, voxel : vec3<i32>, prev : vec3<i32>, t : f32 };

fn bSolid(slot : u32, x : i32, y : i32, z : i32, dim : i32) -> bool {
  if (x < 0 || y < 0 || z < 0 || x >= dim || y >= dim || z >= dim) { return false; }
  return (bodyVox[slot * SLOTVOX + u32(x + y * dim + z * dim * dim)] & 3u) != 0u;
}

// A thin body part (a trunk) is a tiny target, so count the ray as hitting it if
// a voxel is within 2 along any axis -- the carve sphere then covers it.
fn bSolidNear(slot : u32, v : vec3<i32>, dim : i32) -> bool {
  for (var d = -2; d <= 2; d = d + 1) {
    if (bSolid(slot, v.x + d, v.y, v.z, dim)) { return true; }
    if (bSolid(slot, v.x, v.y + d, v.z, dim)) { return true; }
    if (bSolid(slot, v.x, v.y, v.z + d, dim)) { return true; }
  }
  return false;
}

// Two-level brick/voxel DDA, identical to the render march, so the body-vs-world
// depth test the carve makes matches exactly what the render shows on screen.
fn marchWorld(ro : vec3<f32>, rd : vec3<f32>) -> March {
  var m : March;
  m.found = false;
  let inv = vec3<f32>(1.0) / rd;
  let t0 = (vec3<f32>(0.0) - ro) * inv;
  let t1 = (vec3<f32>(f32(WG)) - ro) * inv;
  let tenter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z));
  let texit = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
  if (texit < max(tenter, 0.0)) { return m; }
  let tstart = max(tenter, 0.0) + 0.0001;
  let stepi = vec3<i32>(sign(rd));
  let stepf = vec3<f32>(sign(rd));
  let pos0 = max(stepf, vec3<f32>(0.0));
  var brick = clamp(vec3<i32>(floor((ro + rd * tstart) / 8.0)),
                    vec3<i32>(0), vec3<i32>(BG - 1));
  var tMaxB = ((vec3<f32>(brick) + pos0) * 8.0 - ro) * inv;
  let tDeltaB = abs(inv) * 8.0;
  var tEnter = tstart;
  for (var ob = 0; ob < 210; ob = ob + 1) {
    if (brick.x < 0 || brick.y < 0 || brick.z < 0 || brick.x >= BG || brick.y >= BG || brick.z >= BG) { break; }
    let bidx = u32(brick.x + brick.y * BG + brick.z * BG * BG);
    if (bricks[bidx].occupancy != 0u) {
      let slot = bricks[bidx].pointer;
      let bmin = brick * 8;
      var voxel = clamp(vec3<i32>(floor(ro + rd * (tEnter + 0.0001))), bmin, bmin + vec3<i32>(7));
      var tMaxV = ((vec3<f32>(voxel) + pos0) - ro) * inv;
      var tVox = tEnter;
      var prev = voxel;
      for (var iv = 0; iv < 26; iv = iv + 1) {
        let l = voxel - bmin;
        if ((voxCur[slot * 512u + u32(l.x + l.y * 8 + l.z * 64)] & 3u) != 0u) {
          m.found = true; m.voxel = voxel; m.prev = prev; m.t = tVox; return m;
        }
        prev = voxel;
        if (tMaxV.x < tMaxV.y && tMaxV.x < tMaxV.z) {
          tVox = tMaxV.x; voxel.x += stepi.x; tMaxV.x += abs(inv.x);
          if (voxel.x < bmin.x || voxel.x > bmin.x + 7) { break; }
        } else if (tMaxV.y < tMaxV.z) {
          tVox = tMaxV.y; voxel.y += stepi.y; tMaxV.y += abs(inv.y);
          if (voxel.y < bmin.y || voxel.y > bmin.y + 7) { break; }
        } else {
          tVox = tMaxV.z; voxel.z += stepi.z; tMaxV.z += abs(inv.z);
          if (voxel.z < bmin.z || voxel.z > bmin.z + 7) { break; }
        }
      }
    }
    if (tMaxB.x < tMaxB.y && tMaxB.x < tMaxB.z) {
      brick.x += stepi.x; tEnter = tMaxB.x; tMaxB.x += tDeltaB.x;
    } else if (tMaxB.y < tMaxB.z) {
      brick.y += stepi.y; tEnter = tMaxB.y; tMaxB.y += tDeltaB.y;
    } else {
      brick.z += stepi.z; tEnter = tMaxB.z; tMaxB.z += tDeltaB.z;
    }
  }
  return m;
}

// March body `slot`'s local grid; t is in world units (orthonormal R), so it is
// directly comparable to the world march t. Returns the hit body-local voxel.
fn marchBody(ro : vec3<f32>, rd : vec3<f32>, slot : u32) -> March {
  var m : March;
  m.found = false;
  let body = bodies[slot];
  let dim = i32(body.flags.y);
  let R = mat3x3<f32>(body.invRot0.xyz, body.invRot1.xyz, body.invRot2.xyz);
  let rol = R * (ro - body.center.xyz) + body.pivot.xyz;
  let rdl = R * rd;
  let inv = vec3<f32>(1.0) / rdl;
  let t0 = (vec3<f32>(0.0) - rol) * inv;
  let t1 = (vec3<f32>(f32(dim)) - rol) * inv;
  let tenter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z));
  let texit = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
  if (texit < max(tenter, 0.0)) { return m; }
  let tstart = max(tenter, 0.0) + 0.0001;
  let stp = vec3<i32>(sign(rdl));
  let td = abs(inv);
  var voxel = clamp(vec3<i32>(floor(rol + rdl * tstart)), vec3<i32>(0), vec3<i32>(dim - 1));
  var tm = (vec3<f32>(f32(voxel.x) + select(0.0, 1.0, stp.x > 0),
                      f32(voxel.y) + select(0.0, 1.0, stp.y > 0),
                      f32(voxel.z) + select(0.0, 1.0, stp.z > 0)) - rol) * inv;
  var tHit = tstart;
  for (var i = 0; i < 220; i = i + 1) {
    if (voxel.x < 0 || voxel.y < 0 || voxel.z < 0 || voxel.x >= dim || voxel.y >= dim || voxel.z >= dim) { break; }
    if (bSolidNear(slot, voxel, dim)) {
      m.found = true; m.voxel = voxel; m.prev = voxel; m.t = tHit; return m;
    }
    if (tm.x < tm.y && tm.x < tm.z) { tHit = tm.x; voxel.x += stp.x; tm.x += td.x; }
    else if (tm.y < tm.z) { tHit = tm.y; voxel.y += stp.y; tm.y += td.y; }
    else { tHit = tm.z; voxel.z += stp.z; tm.z += td.z; }
  }
  return m;
}

var<workgroup> hit : vec3<i32>;
var<workgroup> pre : vec3<i32>;
var<workgroup> found : u32;
var<workgroup> hitSlot : u32; // 0xFFFFFFFF = world, else a body slot

@compute @workgroup_size(64)
fn edit(@builtin(local_invocation_index) lid : u32) {
  if (lid == 0u) {
    found = 0u;
    hitSlot = 0xFFFFFFFFu;
    let ro = ed.v0.xyz;
    let rd = normalize(ed.v1.xyz);
    var bestT = 1.0e30;
    let w = marchWorld(ro, rd);
    if (w.found) { found = 1u; bestT = w.t; hit = w.voxel; pre = w.prev; }
    // A carveable body overrides the world hit if it is nearer.
    for (var s = 0u; s < MAXB; s = s + 1u) {
      if (bodies[s].flags.x == 0u || bodies[s].flags.w == 0u) { continue; }
      let b = marchBody(ro, rd, s);
      if (b.found && b.t < bestT) {
        found = 1u; bestT = b.t; hit = b.voxel; pre = b.prev; hitSlot = s;
      }
    }
    // Report the carve target so the body-split pass knows which grid to label,
    // and the world hit voxel so the window fell can position its box.
    carveHit[0] = select(0u, 1u, hitSlot != 0xFFFFFFFFu);
    carveHit[1] = hitSlot;
    carveHit[2] = u32(hit.x);
    carveHit[3] = u32(hit.y);
    carveHit[4] = u32(hit.z);
  }
  workgroupBarrier();
  if (found == 0u) { return; }
  let mode = u32(ed.v1.w);
  let R = i32(ed.v0.w);
  let center = select(hit, pre, mode == 2u);
  let dim = 2 * R + 1;
  let total = dim * dim * dim;

  if (hitSlot != 0xFFFFFFFFu) {
    // Body hit: carve its local grid (single buffer, no brickmap/dirty). Water
    // spawn is world-only, so a body hit always carves.
    let bdim = i32(bodies[hitSlot].flags.y);
    for (var idx = i32(lid); idx < total; idx = idx + 64) {
      let dx = idx % dim - R;
      let dy = (idx / dim) % dim - R;
      let dz = idx / (dim * dim) - R;
      if (dx * dx + dy * dy + dz * dz > R * R) { continue; }
      let lx = center.x + dx; let ly = center.y + dy; let lz = center.z + dz;
      if (lx < 0 || ly < 0 || lz < 0 || lx >= bdim || ly >= bdim || lz >= bdim) { continue; }
      bodyVox[hitSlot * SLOTVOX + u32(lx + ly * bdim + lz * bdim * bdim)] = 0u;
    }
    return;
  }

  for (var idx = i32(lid); idx < total; idx = idx + 64) {
    let lx = idx % dim;
    let ly = (idx / dim) % dim;
    let lz = idx / (dim * dim);
    let dx = lx - R; let dy = ly - R; let dz = lz - R;
    if (dx * dx + dy * dy + dz * dz > R * R) { continue; }
    let wx = center.x + dx; let wy = center.y + dy; let wz = center.z + dz;
    if (!inW(wx, wy, wz)) { continue; }
    let i = vIdx(wx, wy, wz);
    if (mode == 1u) {
      voxCur[i] = 0u; voxOther[i] = 0u;
      atomicOr(&dirty[u32((wx / 8) + (wy / 8) * BG + (wz / 8) * BG * BG)], 1u);
    } else if ((voxCur[i] & 3u) == 0u) {
      let dir = hash3(u32(wx), u32(wy), u32(wz), u32(idx)) & 3u;
      voxCur[i] = (50u << 24u) | (110u << 16u) | (210u << 8u) | 2u | (dir << 2u);
    }
  }
}
