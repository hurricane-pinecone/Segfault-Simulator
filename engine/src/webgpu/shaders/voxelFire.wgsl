// Fire cellular automaton. Each frame, over occupied bricks, flammable SOLID voxels
// burn: a burning voxel probabilistically burns out (palette burnoutRate) to char,
// falling char powder, or air (leaves), occasionally puffs smoke; a non-burning
// flammable voxel ignites if a neighbour is burning (PULL model, so each cell writes
// only itself -- race-tolerant; fire is chaotic, slight timing races don't matter).
// Solid-state changes write BOTH voxel buffers (solids live in both); smoke goes to
// the current src only (it's dynamic, ping-ponged by the water CA). When fire REMOVES
// a static solid it APPENDS the cell to the world-fell work-list (fellList), so a
// burnt-through trunk drops its canopy as a rigid body -- felling is driven by voxel
// removal, not by the carve tool, and many trees fell independently (no shared center
// contention with the carve tool).
@group(0) @binding(0) var<storage, read_write> voxCur : array<u32>; // src
@group(0) @binding(1) var<storage, read_write> voxOther : array<u32>;
@group(0) @binding(2) var<storage, read> bricks : array<Brick>;
@group(0) @binding(3) var<uniform> frame : vec4<u32>;
@group(0) @binding(4) var<storage, read> materials : array<Material>;
// Fell work-list: [0] = atomic count, then FELL_MAX packed (x,y,z) u32 centres.
@group(0) @binding(6) var<storage, read_write> fellList : array<atomic<u32>>;

fn vIdx(x : i32, y : i32, z : i32) -> u32 {
  let bi = u32((x / 8) + (y / 8) * BG + (z / 8) * BG * BG);
  return bricks[bi].pointer * 512u + u32((x % 8) + (y % 8) * 8 + (z % 8) * 64);
}
fn worldAt(x : i32, y : i32, z : i32) -> u32 {
  if (x < 0 || y < 0 || z < 0 || x >= WG || y >= WG || z >= WG) { return 0u; }
  let bi = u32((x / 8) + (y / 8) * BG + (z / 8) * BG * BG);
  if (bricks[bi].occupancy == 0u) { return 0u; }
  return voxCur[vIdx(x, y, z)];
}
fn nbBurning(x : i32, y : i32, z : i32) -> bool {
  return isBurning(worldAt(x, y, z)); // burning fuel, solid or powder
}
fn solidNbCount(x : i32, y : i32, z : i32) -> u32 {
  var c = 0u;
  if ((worldAt(x - 1, y, z) & 3u) == CAT_SOLID) { c = c + 1u; }
  if ((worldAt(x + 1, y, z) & 3u) == CAT_SOLID) { c = c + 1u; }
  if ((worldAt(x, y - 1, z) & 3u) == CAT_SOLID) { c = c + 1u; }
  if ((worldAt(x, y + 1, z) & 3u) == CAT_SOLID) { c = c + 1u; }
  if ((worldAt(x, y, z - 1) & 3u) == CAT_SOLID) { c = c + 1u; }
  if ((worldAt(x, y, z + 1) & 3u) == CAT_SOLID) { c = c + 1u; }
  return c;
}
fn rnd(x : i32, y : i32, z : i32, salt : u32) -> f32 {
  return f32(hash3(u32(x), u32(y), u32(z), frame.x + salt) & 0xFFFFu) / 65535.0;
}
// Append a sever cell to the world-fell work-list. The CPU drains the list one frame
// late, running a 96^3 window centred on each entry, so many trees fell independently
// and fire never contends with the carve tool for a single shared centre. The window's
// within-brick CC separates the severed-above part from the still-grounded-below part.
// Probability-gated at the call site (the window is large -- one append near a burning
// region per frame fells the whole tree -- so flooding the list with every crumble is
// wasteful).
fn seedFell(x : i32, y : i32, z : i32) {
  let i = atomicAdd(&fellList[0], 1u);
  if (i < FELL_MAX) {
    let b = 1u + i * 3u;
    atomicStore(&fellList[b], u32(x));
    atomicStore(&fellList[b + 1u], u32(y));
    atomicStore(&fellList[b + 2u], u32(z));
  }
}
@compute @workgroup_size(8, 8, 1)
fn fire(@builtin(local_invocation_id) lloc : vec3<u32>,
        @builtin(workgroup_id) wid : vec3<u32>) {
  let dt = bitcast<f32>(frame.y); // seconds since last frame (rate * dt = per-frame prob)
  let x = i32(wid.x) * 8 + i32(lloc.x);
  let y = i32(wid.y) * 8 + i32(lloc.y);
  for (var lz = 0; lz < 8; lz = lz + 1) {
    let z = i32(wid.z) * 8 + lz;
    let v = voxCur[vIdx(x, y, z)];
    let cat = v & 3u;
    // Fuel = a flammable SOLID (terrain/wood/leaves) or a flammable POWDER (e.g.
    // crumbled leaves, debris rubble). Powder is dynamic: its fire writes src only
    // and the burning state rides along as the water CA moves the voxel.
    let powder = cat == CAT_LIQUID && (v & VOX_POWDER) != 0u;
    if (cat != CAT_SOLID && !powder) { continue; }
    let m = matId(v);
    let fire = materials[m].fire; // x catchRate, y burnoutRate, z crumbleChance
    let flam = fire.x > 0.0;
    let i = vIdx(x, y, z);

    if (isBurning(v)) {
      // Probabilistic burnout (no timer): a low burnoutRate burns long. On burnout a
      // solid either crumbles to falling powder (crumbleChance) -- so a torched tree
      // collapses -- or chars in place; leaves are consumed to air; burning powder is
      // consumed.
      if (rnd(x, y, z, 7u) < fire.y * dt) {
        if (powder) {
          voxCur[i] = 0u;
        } else if (m == MAT_LEAVES) {
          voxCur[i] = 0u;
          voxOther[i] = 0u;
        } else if (rnd(x, y, z, 8u) < fire.z) {
          voxCur[i] = vox(MAT_CHAR, CAT_LIQUID) | VOX_POWDER; // falls
          voxOther[i] = 0u;                                   // no longer static
          // Removing load-bearing wood may have severed the canopy -> seed the fell.
          if (m == MAT_TRUNK && rnd(x, y, z, 41u) < 0.04) { seedFell(x, y, z); }
        } else {
          let burnt = vox(MAT_CHAR, CAT_SOLID);
          voxCur[i] = burnt;
          voxOther[i] = burnt;
        }
      }
      // Puff smoke into the air cell above (dynamic -> src only). ~3 puffs/sec.
      if (rnd(x, y, z, 11u) < 3.0 * dt && worldAt(x, y + 1, z) == 0u &&
          y + 1 < WG) {
        let life = 60u + (hash3(u32(x), u32(y), u32(z), frame.x) % 90u);
        voxCur[vIdx(x, y + 1, z)] = vox(MAT_SMOKE, CAT_GAS) | (life << 16u);
      }
    } else {
      var ignited = false;
      // Pull ignition: catch from any burning neighbour (catchRate per second).
      if (flam && (nbBurning(x - 1, y, z) || nbBurning(x + 1, y, z) ||
                   nbBurning(x, y - 1, z) || nbBurning(x, y + 1, z) ||
                   nbBurning(x, y, z - 1) || nbBurning(x, y, z + 1))) {
        if (rnd(x, y, z, 23u) < fire.x * dt) {
          let nv = v | VOX_BURNING;
          voxCur[i] = nv;
          if (!powder) { voxOther[i] = nv; }
          ignited = true;
        }
      }
      // Crumble undermined scraps to falling powder of their own material; the fluid
      // CA carries them down and neighbours cascade -- this is how a burnt tree falls
      // apart. Live wood/leaves only go when truly isolated (<=1 neighbour) so healthy
      // trees stay whole, but brittle CHAR crumbles at <=2 so a thin charred bridge
      // across a burnt-through trunk breaks instead of holding the canopy up forever.
      let isoNb = solidNbCount(x, y, z);
      let isolated = (flam && isoNb <= 1u) || (m == MAT_CHAR && isoNb <= 2u);
      if (!ignited && !powder && isolated) {
        voxCur[i] = vox(m, CAT_LIQUID) | VOX_POWDER;
        voxOther[i] = 0u;
        // Trunk/char crumbling can sever the canopy -> seed the fell.
        if ((m == MAT_TRUNK || m == MAT_CHAR) && rnd(x, y, z, 41u) < 0.04) {
          seedFell(x, y, z);
        }
      }
    }
  }
}
