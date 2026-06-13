// Fluid cellular automaton (liquids + gases). Each fluid voxel first tries to
// move in its preferred vertical direction -- liquids fall, gases rise -- then
// flows sideways (remembered direction, else a new random one). Each move claims
// a cleared destination via compare-exchange (conserved, race-free). Static
// terrain + air persist in dst, so only fluids act. Liquids fall one cell/tick;
// gases rise stochastically at a density-driven, per-voxel-jittered rate (slower
// than a cell/tick, uneven).
@group(0) @binding(0) var<storage, read> src : array<u32>;
@group(0) @binding(1) var<storage, read_write> dst : array<atomic<u32>>;
@group(0) @binding(2) var<storage, read> bricks : array<Brick>;
@group(0) @binding(3) var<uniform> frame : vec4<u32>;
@group(0) @binding(4) var<storage, read> materials : array<Material>;

// Gas rise is stochastic: each tick a gas voxel rises with a probability set by its
// material density and a per-voxel jitter, so it rises slower than 1 cell/tick and
// unevenly (some wisps outrun others). Lower density -> lower probability -> slower.
const RISE_SCALE : f32 = 1.4;     // density -> base rise probability
const RISE_JIT_LO : f32 = 0.4;    // per-voxel rate range = base * [LO, LO+SPAN]
const RISE_JIT_SPAN : f32 = 1.4;
fn rnd01(x : i32, y : i32, z : i32, salt : u32) -> f32 {
  return f32(hash3(u32(x), u32(y), u32(z), frame.x + salt) & 0xFFFFu) / 65535.0;
}

fn vIndex(x : i32, y : i32, z : i32) -> u32 {
  let bi = u32((x / 8) + (y / 8) * BG + (z / 8) * BG * BG);
  return bricks[bi].pointer * 512u + u32((x % 8) + (y % 8) * 8 + (z % 8) * 64);
}
fn srcType(x : i32, y : i32, z : i32) -> u32 {
  if (x < 0 || y < 0 || z < 0 || x >= WG || y >= WG || z >= WG) { return 1u; } // wall
  return src[vIndex(x, y, z)] & 3u;
}
fn nbx(x : i32, d : u32) -> i32 { if (d == 0u) { return x + 1; } if (d == 1u) { return x - 1; } return x; }
fn nbz(z : i32, d : u32) -> i32 { if (d == 2u) { return z + 1; } if (d == 3u) { return z - 1; } return z; }

@compute @workgroup_size(8, 8, 1)
fn water(@builtin(local_invocation_id) lloc : vec3<u32>,
         @builtin(workgroup_id) wid : vec3<u32>) {
  let x = i32(wid.x) * 8 + i32(lloc.x);
  let y = i32(wid.y) * 8 + i32(lloc.y);
  for (var lz = 0; lz < 8; lz = lz + 1) {
    let z = i32(wid.z) * 8 + lz;
    let v = src[vIndex(x, y, z)];
    let ty = v & 3u;
    let isGas = ty == CAT_GAS;
    if (ty != CAT_LIQUID && !isGas) { continue; }

    // Powder (rubble): falls straight, else slides one cell diagonally DOWN to
    // pile at an angle, else rests. (Liquid category + powder flag.)
    if (ty == CAT_LIQUID && (v & VOX_POWDER) != 0u) {
      var movedP = false;
      if (srcType(x, y - 1, z) == 0u) {
        let r = atomicCompareExchangeWeak(&dst[vIndex(x, y - 1, z)], 0u, v);
        if (r.exchanged) { movedP = true; }
      }
      if (!movedP) {
        let pk = hash3(u32(x), u32(y), u32(z), frame.x) % 4u;
        for (var k = 0u; k < 4u; k = k + 1u) {
          let d = (pk + k) % 4u;
          let nx = nbx(x, d);
          let nz = nbz(z, d);
          if (srcType(nx, y - 1, nz) == 0u) {
            let r2 = atomicCompareExchangeWeak(&dst[vIndex(nx, y - 1, nz)], 0u, v);
            if (r2.exchanged) { movedP = true; break; }
          }
        }
      }
      if (!movedP) { atomicStore(&dst[vIndex(x, y, z)], v); } // piled -> rest
      continue;
    }

    // Gas: dissipate (decrement lifetime, bits 16-23; at 0 -> air), then rise
    // stochastically at a density-driven rate. If it doesn't rise this tick it
    // rests in place (slow rise); if genuinely blocked above by solid it billows
    // sideways.
    if (isGas) {
      let life = (v >> 16u) & 0xFFu;
      if (life == 0u) { continue; }
      let vv = (v & 0xFF00FFFFu) | ((life - 1u) << 16u);
      let above = srcType(x, y + 1, z);
      let dens = materials[matId(v)].density;
      let riseP = clamp(dens * RISE_SCALE * (RISE_JIT_LO + RISE_JIT_SPAN * rnd01(x, y, z, 17u)),
                        0.0, 1.0);
      if (above == 0u && rnd01(x, y, z, 53u) < riseP) {
        let r = atomicCompareExchangeWeak(&dst[vIndex(x, y + 1, z)], 0u, vv);
        if (r.exchanged) { continue; }
      }
      // Only billow sideways against a SOLID ceiling. If blocked by other gas, REST
      // and wait for it to rise -- otherwise the dense cloud all flows sideways and
      // flattens into a cone instead of rising as a plume.
      if (above != CAT_SOLID) { atomicStore(&dst[vIndex(x, y, z)], vv); continue; }
      // Solid ceiling above -> billow sideways (remembered dir, else a new random one).
      let gdir = (v >> 2u) & 3u;
      if (srcType(nbx(x, gdir), y, nbz(z, gdir)) == 0u) {
        let rs = atomicCompareExchangeWeak(&dst[vIndex(nbx(x, gdir), y, nbz(z, gdir))], 0u, vv);
        if (rs.exchanged) { continue; }
      }
      let gp = hash3(u32(x), u32(y), u32(z), frame.x) % 4u;
      for (var k = 0u; k < 4u; k = k + 1u) {
        let d = (gp + k) % 4u;
        if (srcType(nbx(x, d), y, nbz(z, d)) == 0u) {
          let nv = (vv & 0xFFFFFFF3u) | (d << 2u);
          let r2 = atomicCompareExchangeWeak(&dst[vIndex(nbx(x, d), y, nbz(z, d))], 0u, nv);
          if (r2.exchanged) { break; }
        }
      }
      atomicStore(&dst[vIndex(x, y, z)], vv);
      continue;
    }

    let dy = -1; // liquids fall
    let vv = v;

    // Move in the preferred vertical direction if it can; claims a cleared cell.
    if (srcType(x, y + dy, z) == 0u) {
      let r = atomicCompareExchangeWeak(&dst[vIndex(x, y + dy, z)], 0u, vv);
      if (r.exchanged) { continue; }
    }
    // Else keep flowing in the remembered direction until it's blocked...
    let dir = (v >> 2u) & 3u;
    let sx = nbx(x, dir);
    let sz = nbz(z, dir);
    if (srcType(sx, y, sz) == 0u) {
      let rs = atomicCompareExchangeWeak(&dst[vIndex(sx, y, sz)], 0u, vv);
      if (rs.exchanged) { continue; }
    }
    // ...blocked: pick a new random direction and remember it.
    var moved = false;
    let pick = hash3(u32(x), u32(y), u32(z), frame.x) % 4u;
    for (var k = 0u; k < 4u; k = k + 1u) {
      let d = (pick + k) % 4u;
      let nx = nbx(x, d);
      let nz = nbz(z, d);
      if (srcType(nx, y, nz) == 0u) {
        let nv = (vv & 0xFFFFFFF3u) | (d << 2u);
        let r2 = atomicCompareExchangeWeak(&dst[vIndex(nx, y, nz)], 0u, nv);
        if (r2.exchanged) { moved = true; break; }
      }
    }
    if (moved) { continue; }
    atomicStore(&dst[vIndex(x, y, z)], vv); // couldn't move -> stay
  }
}
