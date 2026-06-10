// Water cellular automaton: each water voxel falls if it can, else keeps its
// remembered flow direction, else picks a new random direction. Each move claims
// a cleared destination cell via compare-exchange (conserved, race-free). Static
// terrain + air persist in dst (never cleared or copied), so only water acts.
@group(0) @binding(0) var<storage, read> src : array<u32>;
@group(0) @binding(1) var<storage, read_write> dst : array<atomic<u32>>;
@group(0) @binding(2) var<storage, read> bricks : array<Brick>;
@group(0) @binding(3) var<uniform> frame : vec4<u32>;

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
    if (ty != 2u) { continue; }

    // Fall if it can; each move claims a cleared destination cell.
    if (srcType(x, y - 1, z) == 0u) {
      let r = atomicCompareExchangeWeak(&dst[vIndex(x, y - 1, z)], 0u, v);
      if (r.exchanged) { continue; }
    }
    // Else keep flowing in the remembered direction until it's blocked...
    let dir = (v >> 2u) & 3u;
    let sx = nbx(x, dir);
    let sz = nbz(z, dir);
    if (srcType(sx, y, sz) == 0u) {
      let rs = atomicCompareExchangeWeak(&dst[vIndex(sx, y, sz)], 0u, v);
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
        let nv = (v & 0xFFFFFFF3u) | (d << 2u);
        let r2 = atomicCompareExchangeWeak(&dst[vIndex(nx, y, nz)], 0u, nv);
        if (r2.exchanged) { moved = true; break; }
      }
    }
    if (moved) { continue; }
    atomicStore(&dst[vIndex(x, y, z)], v); // couldn't move -> stay
  }
}
