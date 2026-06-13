// Ballistic debris advection + collision. One thread per pool slot: integrate
// gravity + drag, sub-step the path (no tunnelling through thin walls), and on the
// first solid cell decide by kinetic energy vs the target's toughness:
//   KE  = 1/2 * density(self) * |vel|^2     (mass from the carried material)
//   TGH = rigidity(target) * K + base
//   KE > TGH  -> BREAK the world voxel; the debris loses energy and penetrates,
//               and (above a spall threshold) the broken material flies off as a
//               new debris, so impacts cascade (bounded by the ring + energy loss).
//   else      -> SETTLE: the carried material lands as powder (the fluid CA piles
//               it) and the slot frees.
// Carries the full voxel value, so density/rigidity/colour (flammability later) all
// come from the material palette -- the same seam actors plug into in M2b.
@group(0) @binding(0) var<storage, read_write> debris : array<Debris>;
@group(0) @binding(1) var<storage, read_write> voxCur : array<u32>;  // src
@group(0) @binding(2) var<storage, read> bricks : array<Brick>;
@group(0) @binding(3) var<uniform> stepU : vec4<f32>;                 // x = dt
@group(0) @binding(4) var<storage, read> materials : array<Material>;
@group(0) @binding(5) var<storage, read_write> debrisHead : array<atomic<u32>>;
@group(0) @binding(6) var<storage, read_write> voxOther : array<u32>; // the other buffer
// Rigid bodies (transforms RO) + their sparse brickmap (grid RO, pool RW so debris
// can chip body voxels). Same broad-phase seam actors will plug into.
struct Body {
  flags : vec4<u32>,   // x active, y dim
  invRot0 : vec4<f32>, // world -> local rotation columns
  invRot1 : vec4<f32>,
  invRot2 : vec4<f32>,
  center : vec4<f32>,  // world CoM
  pivot : vec4<f32>,
};
@group(0) @binding(7) var<storage, read> bodies : array<Body, MAXB>;
@group(0) @binding(8) var<storage, read> bodyBrickGrid : array<u32>;
@group(0) @binding(9) var<storage, read_write> bodyBrickPool : array<u32>;

// Toughness is high so debris mostly SETTLE as rubble -- only a genuinely hard,
// fast, head-on impact craters terrain. (Low toughness made debris dissolve the
// surface on contact, which reads as corrosion, not kinetic damage -- see the acid
// effect idea in memory.)
// Toughness is scaled so terrain (rigidity >= 1) out-toughens even fresh fast
// debris -> they settle as rubble, no surface melting; only soft bodies (leaves,
// rigidity ~0.08) get chipped. Raise TOUGH_K with eject speed (KE ~ v^2).
const DRAG : f32 = 0.992;
// High headroom so debris never break terrain (rigidity >= 1) even as blast force
// (and thus debris KE ~ v^2) climbs -- terrain destruction is the crater's job,
// debris just settle as rubble. Soft bodies (leaves, rigidity ~0.08) still chip.
const TOUGH_K : f32 = 150000.0;   // rigidity -> toughness scale
const TOUGH_BASE : f32 = 2000.0;  // floor toughness
const ENERGY_RETAIN : f32 = 0.3;  // velocity kept after breaking a voxel
const SPALL_EXCESS : f32 = 3.0;   // spall the broken voxel only when KE > TGH * this
const SETTLE_SPEED2 : f32 = 16.0; // |vel|^2 below which a debris settles

fn vIdx(x : i32, y : i32, z : i32) -> u32 {
  let bi = u32((x / 8) + (y / 8) * BG + (z / 8) * BG * BG);
  return bricks[bi].pointer * 512u + u32((x % 8) + (y % 8) * 8 + (z % 8) * 64);
}
fn inW(c : vec3<i32>) -> bool {
  return c.x >= 0 && c.y >= 0 && c.z >= 0 && c.x < WG && c.y < WG && c.z < WG;
}
fn worldVox(c : vec3<i32>) -> u32 {
  let bi = u32((c.x / 8) + (c.y / 8) * BG + (c.z / 8) * BG * BG);
  if (bricks[bi].occupancy == 0u) { return 0u; }
  return voxCur[vIdx(c.x, c.y, c.z)];
}
fn isSolid(c : vec3<i32>) -> bool {
  if (!inW(c)) { return false; }
  return (worldVox(c) & 3u) == 1u;
}
fn density(m : u32) -> f32 { return max(materials[m].density, 0.1); }

// Land the carried material as powder in an air cell (never overwrite solid).
// A matId-0 voxel would settle as a BLACK powder (palette slot 0 is air/unset) --
// drop it instead of polluting the world.
fn settleAt(c : vec3<i32>, v : u32) {
  if (matId(v) == 0u) { return; }
  if (inW(c) && !isSolid(c)) {
    voxCur[vIdx(c.x, c.y, c.z)] = vox(matId(v), CAT_LIQUID) | VOX_POWDER;
  }
}
fn spawnDebris(p : vec3<f32>, vel : vec3<f32>, v : u32) {
  if (matId(v) == 0u) { return; } // never spawn an air/unset (black) debris
  let s = atomicAdd(&debrisHead[0], 1u) % DEBRIS_MAX;
  debris[s].a = vec4<f32>(p, 3.0);
  debris[s].b = vec4<f32>(vel, bitcast<f32>(v));
}
fn bodyVoxLoad(slot : u32, lx : i32, ly : i32, lz : i32) -> u32 {
  let bp = bodyBrickGrid[slot * BODYBRICKS + brickCell(lx, ly, lz)];
  if (bp == BRICK_EMPTY) { return 0u; }
  return bodyBrickPool[bp * 512u + brickLocal(lx, ly, lz)];
}
fn bodyVoxBreak(slot : u32, lx : i32, ly : i32, lz : i32) {
  let bp = bodyBrickGrid[slot * BODYBRICKS + brickCell(lx, ly, lz)];
  if (bp == BRICK_EMPTY) { return; }
  bodyBrickPool[bp * 512u + brickLocal(lx, ly, lz)] = 0u;
}

@compute @workgroup_size(64)
fn advect(@builtin(global_invocation_id) gid : vec3<u32>) {
  let s = gid.x;
  if (s >= DEBRIS_MAX) { return; }
  var d = debris[s];
  if (d.a.w <= 0.0) { return; } // free slot

  let dt = stepU.x;
  let pos = d.a.xyz;
  var vel = d.b.xyz;
  let v = bitcast<u32>(d.b.w);
  let life = d.a.w - dt;
  if (life <= 0.0) { settleAt(vec3<i32>(floor(pos)), v); debris[s].a.w = 0.0; return; }

  vel.y = vel.y - DEBRIS_G * dt;
  vel = vel * DRAG;
  let npos = pos + vel * dt;

  // Sub-step the segment (~0.5 voxel spacing) to catch thin walls.
  let seg = npos - pos;
  let dist = length(seg);
  let nsub = min(i32(ceil(dist * 2.0)) + 1, 24);
  var prevAir = vec3<i32>(floor(pos));
  var hit = false;
  var hc = vec3<i32>(0, 0, 0);
  for (var k = 1; k <= nsub; k = k + 1) {
    let c = vec3<i32>(floor(pos + seg * (f32(k) / f32(nsub))));
    if (isSolid(c)) { hit = true; hc = c; break; }
    if (inW(c)) { prevAir = c; }
  }

  if (hit) {
    let hv = worldVox(hc);
    let ke = 0.5 * density(matId(v)) * dot(vel, vel);
    let tgh = materials[matId(hv)].rigidity * TOUGH_K + TOUGH_BASE;
    if (ke > tgh) {
      // Break the struck world voxel from BOTH buffers (static terrain lives in
      // both; clearing only src would let it reappear after the next swap).
      let i = vIdx(hc.x, hc.y, hc.z);
      voxCur[i] = 0u;
      voxOther[i] = 0u;
      // Spall: above the excess threshold the broken material flies off as a new
      // debris, scaled by the leftover energy, so hard hits cascade.
      if (ke > tgh * SPALL_EXCESS) {
        let h = hash3(u32(hc.x), u32(hc.y), u32(hc.z), bitcast<u32>(pos.y));
        let jit = vec3<f32>(f32(h & 0xFFu), f32((h >> 8u) & 0xFFu),
                            f32((h >> 16u) & 0xFFu)) / 255.0 - 0.5;
        let sv = (jit * 2.0 + vec3<f32>(0.0, 0.4, 0.0)) * length(vel) * 0.5;
        spawnDebris(vec3<f32>(hc) + 0.5, sv, hv);
      }
      vel = vel * ENERGY_RETAIN;
      if (dot(vel, vel) < SETTLE_SPEED2) {
        settleAt(prevAir, v); debris[s].a.w = 0.0; return;
      }
      // Penetrate: pause just before the broken cell, keep flying next frame.
      debris[s].a = vec4<f32>(vec3<f32>(prevAir) + 0.5, life);
      debris[s].b = vec4<f32>(vel, d.b.w);
      return;
    }
    // Too weak to break -> stop here as powder.
    settleAt(prevAir, v); debris[s].a.w = 0.0; return;
  }

  // No world hit -> test rigid bodies at npos. Broad-phase: a per-body sphere
  // reject, then a body-local voxel check only for the few that overlap. On a hit
  // the debris CHIPS the chunk (breaks a body voxel + spalls it) and bounces off.
  for (var bs2 = 0u; bs2 < MAXB; bs2 = bs2 + 1u) {
    let body = bodies[bs2];
    if (body.flags.x == 0u) { continue; }
    let dim = i32(body.flags.y);
    let dc = npos - body.center.xyz;
    let rad = f32(dim) * 0.87;
    let dd = dot(dc, dc);
    if (dd > rad * rad) { continue; } // sphere reject
    // Outward surface normal. Guard the degenerate case (debris penetrated to the
    // body centre, dc ~ 0): a raw normalize would be NaN -> NaN velocity/position ->
    // a garbage cube stuck on screen. Fall back to bouncing straight back.
    let nrm = select(dc * inverseSqrt(dd), vec3<f32>(0.0, 1.0, 0.0), dd < 0.0001);
    if (dot(vel, nrm) >= 0.0) { continue; } // moving away, not into it
    // world -> body-local
    let R = mat3x3<f32>(body.invRot0.xyz, body.invRot1.xyz, body.invRot2.xyz);
    let lc = vec3<i32>(floor(R * dc + body.pivot.xyz));
    if (lc.x < 0 || lc.y < 0 || lc.z < 0 || lc.x >= dim || lc.y >= dim || lc.z >= dim) { continue; }
    let bv = bodyVoxLoad(bs2, lc.x, lc.y, lc.z);
    if ((bv & 3u) != 1u) { continue; } // empty body cell

    let ke = 0.5 * density(matId(v)) * dot(vel, vel);
    let tgh = materials[matId(bv)].rigidity * TOUGH_K + TOUGH_BASE;
    if (ke > tgh) {
      bodyVoxBreak(bs2, lc.x, lc.y, lc.z);
      if (ke > tgh * SPALL_EXCESS) {
        let h = hash3(u32(lc.x), u32(lc.y), u32(lc.z), bs2);
        let jit = vec3<f32>(f32(h & 0xFFu), f32((h >> 8u) & 0xFFu),
                            f32((h >> 16u) & 0xFFu)) / 255.0 - 0.5;
        spawnDebris(npos, reflect(vel, nrm) * 0.4 + jit * length(vel) * 0.3, bv);
      }
    }
    vel = reflect(vel, nrm) * ENERGY_RETAIN;
    if (dot(vel, vel) < SETTLE_SPEED2) {
      settleAt(vec3<i32>(floor(pos)), v); debris[s].a.w = 0.0; return;
    }
    debris[s].a = vec4<f32>(pos, life); // bounce: stay outside, fly off next frame
    debris[s].b = vec4<f32>(vel, d.b.w);
    return;
  }

  if (npos.y < 1.0) { settleAt(prevAir, v); debris[s].a.w = 0.0; return; }
  if (!inW(vec3<i32>(floor(npos)))) { debris[s].a.w = 0.0; return; } // flew off
  debris[s].a = vec4<f32>(npos, life);
  debris[s].b = vec4<f32>(vel, d.b.w);
}
