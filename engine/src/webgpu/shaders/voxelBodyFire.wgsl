// Fire CA on rigid bodies + RB<->world boundary coupling. A felled burning chunk
// keeps burning in its own grid AND exchanges fire with the world it touches: each
// body voxel maps to a world cell via the body transform (the same mapping the
// collide pass uses), so a burning body ignites adjacent world fuel and catches
// from adjacent burning world (terrain, trees, or burning powder -- powder lives in
// the world grid, so it's covered for free). The coupling is generic + data-driven
// (flammability from the palette), so fire flows across RB/world/powder naturally,
// no per-material code. (RB<->RB coupling is still TODO; two burning chunks touching
// is rare.) Brick-marched, single-buffered, pull-model ignition (race-tolerant).
struct Body {
  flags : vec4<u32>,   // x active, y dim
  invRot0 : vec4<f32>, // world -> local rotation columns
  invRot1 : vec4<f32>,
  invRot2 : vec4<f32>,
  center : vec4<f32>,  // world CoM
  pivot : vec4<f32>,
};
@group(0) @binding(0) var<storage, read> bodyBrickGrid : array<u32>;
@group(0) @binding(1) var<storage, read_write> bodyBrickPool : array<u32>;
@group(0) @binding(2) var<storage, read> materials : array<Material>;
@group(0) @binding(3) var<uniform> frame : vec4<u32>; // .x hash salt, .y dt (bitcast)
@group(0) @binding(4) var<storage, read> bodies : array<Body, MAXB>;
@group(0) @binding(5) var<storage, read_write> voxCur : array<u32>;  // world src
@group(0) @binding(6) var<storage, read_write> voxOther : array<u32>;
@group(0) @binding(7) var<storage, read> bricks : array<Brick>;      // world occupancy

fn bodyVoxLoad(slot : u32, x : i32, y : i32, z : i32) -> u32 {
  if (x < 0 || y < 0 || z < 0 || x >= BODYDIM || y >= BODYDIM || z >= BODYDIM) { return 0u; }
  let bp = bodyBrickGrid[slot * BODYBRICKS + brickCell(x, y, z)];
  if (bp == BRICK_EMPTY) { return 0u; }
  return bodyBrickPool[bp * 512u + brickLocal(x, y, z)];
}
fn bodyNbBurning(slot : u32, x : i32, y : i32, z : i32) -> bool {
  return isBurning(bodyVoxLoad(slot, x, y, z));
}
fn bodySolidNb(slot : u32, x : i32, y : i32, z : i32) -> u32 {
  var c = 0u;
  if ((bodyVoxLoad(slot, x - 1, y, z) & 3u) == CAT_SOLID) { c = c + 1u; }
  if ((bodyVoxLoad(slot, x + 1, y, z) & 3u) == CAT_SOLID) { c = c + 1u; }
  if ((bodyVoxLoad(slot, x, y - 1, z) & 3u) == CAT_SOLID) { c = c + 1u; }
  if ((bodyVoxLoad(slot, x, y + 1, z) & 3u) == CAT_SOLID) { c = c + 1u; }
  if ((bodyVoxLoad(slot, x, y, z - 1) & 3u) == CAT_SOLID) { c = c + 1u; }
  if ((bodyVoxLoad(slot, x, y, z + 1) & 3u) == CAT_SOLID) { c = c + 1u; }
  return c;
}
fn frnd(x : i32, y : i32, z : i32, slot : u32, salt : u32) -> f32 {
  return f32(hash3(u32(x) + slot * 131u, u32(y), u32(z), frame.x + salt) & 0xFFFFu) / 65535.0;
}
fn wIdx(x : i32, y : i32, z : i32) -> u32 {
  let bi = u32((x / 8) + (y / 8) * BG + (z / 8) * BG * BG);
  return bricks[bi].pointer * 512u + u32((x % 8) + (y % 8) * 8 + (z % 8) * 64);
}
fn worldVoxAt(x : i32, y : i32, z : i32) -> u32 {
  if (x < 0 || y < 0 || z < 0 || x >= WG || y >= WG || z >= WG) { return 0u; }
  let bi = u32((x / 8) + (y / 8) * BG + (z / 8) * BG * BG);
  if (bricks[bi].occupancy == 0u) { return 0u; }
  return voxCur[wIdx(x, y, z)];
}
fn worldNbBurning(c : vec3<i32>) -> bool {
  return isBurning(worldVoxAt(c.x - 1, c.y, c.z)) || isBurning(worldVoxAt(c.x + 1, c.y, c.z)) ||
         isBurning(worldVoxAt(c.x, c.y - 1, c.z)) || isBurning(worldVoxAt(c.x, c.y + 1, c.z)) ||
         isBurning(worldVoxAt(c.x, c.y, c.z - 1)) || isBurning(worldVoxAt(c.x, c.y, c.z + 1));
}
// A burning body voxel ignites an adjacent world SOLID fuel cell (both buffers --
// solids live in both). Powder ignition from a body is deferred.
fn igniteWorld(x : i32, y : i32, z : i32, dt : f32) {
  let wv = worldVoxAt(x, y, z);
  if ((wv & 3u) != CAT_SOLID) { return; }
  let mm = matId(wv);
  let cr = materials[mm].fire.x; // catchRate; >0 = flammable
  if (cr <= 0.0 || isBurning(wv)) { return; }
  if (frnd(x, y, z, 0u, 99u) < cr * dt) {
    let nv = wv | VOX_BURNING;
    let i = wIdx(x, y, z);
    voxCur[i] = nv;
    voxOther[i] = nv;
  }
}
// A body voxel can't live as a "falling" voxel in the rigid grid, so a crumbled or
// isolated scrap is ejected into the world as falling powder at its world cell (if
// that cell is air); the caller then clears it from the body.
fn ejectPowder(wc : vec3<i32>, m : u32) {
  if (wc.x >= 0 && wc.y >= 0 && wc.z >= 0 && wc.x < WG && wc.y < WG && wc.z < WG &&
      (worldVoxAt(wc.x, wc.y, wc.z) & 3u) == 0u) {
    voxCur[wIdx(wc.x, wc.y, wc.z)] = vox(m, CAT_LIQUID) | VOX_POWDER;
  }
}

@compute @workgroup_size(64)
fn bodyFire(@builtin(workgroup_id) wid : vec3<u32>,
            @builtin(local_invocation_index) lidx : u32) {
  let slot = wid.z / u32(BODYBD);
  if (slot >= MAXB) { return; }
  let bz = wid.z % u32(BODYBD);
  let cell = wid.x + wid.y * u32(BODYBD) + bz * u32(BODYBD) * u32(BODYBD);
  let bp = bodyBrickGrid[slot * BODYBRICKS + cell];
  if (bp == BRICK_EMPTY) { return; } // empty / inactive body brick

  let dt = bitcast<f32>(frame.y);
  // body-local -> world transform (same convention as the collide pass).
  let body = bodies[slot];
  let Rlw = transpose(mat3x3<f32>(body.invRot0.xyz, body.invRot1.xyz, body.invRot2.xyz));
  let cx = i32(lidx % 8u); let cy = i32(lidx / 8u);
  let bx = i32(wid.x) * 8; let by = i32(wid.y) * 8; let bmz = i32(bz) * 8;
  for (var cz = 0; cz < 8; cz = cz + 1) {
    let loc = u32(cx + cy * 8 + cz * 64);
    let v = bodyBrickPool[bp * 512u + loc];
    if ((v & 3u) != CAT_SOLID) { continue; }
    let m = matId(v);
    let fire = materials[m].fire; // x catchRate, y burnoutRate, z crumbleChance
    let flam = fire.x > 0.0;
    let lx = bx + cx; let ly = by + cy; let lz = bmz + cz;
    let wc = vec3<i32>(floor(Rlw * (vec3<f32>(f32(lx) + 0.5, f32(ly) + 0.5, f32(lz) + 0.5) - body.pivot.xyz) + body.center.xyz));

    if (isBurning(v)) {
      // Probabilistic burnout (low burnoutRate burns long). Leaves are consumed;
      // other fuel either crumbles to falling char (ejected to the world, since the
      // rigid grid can't hold a "falling" voxel) so a torched chunk sheds, or chars
      // in place.
      if (frnd(lx, ly, lz, slot, 7u) < fire.y * dt) {
        if (m == MAT_LEAVES) {
          bodyBrickPool[bp * 512u + loc] = 0u;
        } else if (frnd(lx, ly, lz, slot, 8u) < fire.z) {
          ejectPowder(wc, MAT_CHAR);
          bodyBrickPool[bp * 512u + loc] = 0u;
        } else {
          bodyBrickPool[bp * 512u + loc] = vox(MAT_CHAR, CAT_SOLID);
        }
      }
      // Coupling out: ignite adjacent world fuel at this voxel's world cell.
      igniteWorld(wc.x - 1, wc.y, wc.z, dt);
      igniteWorld(wc.x + 1, wc.y, wc.z, dt);
      igniteWorld(wc.x, wc.y - 1, wc.z, dt);
      igniteWorld(wc.x, wc.y + 1, wc.z, dt);
      igniteWorld(wc.x, wc.y, wc.z - 1, dt);
      igniteWorld(wc.x, wc.y, wc.z + 1, dt);
    } else {
      var ignited = false;
      // Catch from a burning body OR world neighbour (catchRate per second).
      if (flam && (bodyNbBurning(slot, lx - 1, ly, lz) || bodyNbBurning(slot, lx + 1, ly, lz) ||
          bodyNbBurning(slot, lx, ly - 1, lz) || bodyNbBurning(slot, lx, ly + 1, lz) ||
          bodyNbBurning(slot, lx, ly, lz - 1) || bodyNbBurning(slot, lx, ly, lz + 1) ||
          worldNbBurning(wc))) {
        if (frnd(lx, ly, lz, slot, 23u) < fire.x * dt) {
          bodyBrickPool[bp * 512u + loc] = v | VOX_BURNING;
          ignited = true;
        }
      }
      // A flammable-or-char body voxel with <=1 solid body neighbour is a floating
      // scrap (its support burnt away) -> eject to falling world powder of its own
      // material; this is how a burnt body sheds and collapses.
      if (!ignited && (flam || m == MAT_CHAR) && bodySolidNb(slot, lx, ly, lz) <= 1u) {
        ejectPowder(wc, m);
        bodyBrickPool[bp * 512u + loc] = 0u;
      }
    }
  }
}
