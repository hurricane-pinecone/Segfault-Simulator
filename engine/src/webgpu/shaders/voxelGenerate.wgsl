// World generator: value-noise foothills, grass/sand/dirt/stone bands, lakes,
// and trees. Writes terrain into both voxel buffers (static lives in both); lake
// water only into buffer 0. Also seeds each brick's occupancy count.
@group(0) @binding(0) var<storage, read_write> voxels0 : array<u32>;
@group(0) @binding(1) var<storage, read_write> voxels1 : array<u32>;
@group(0) @binding(2) var<storage, read_write> bricks : array<Brick>;

fn hash22(p : vec2<f32>) -> f32 {
  return fract(sin(dot(p, vec2<f32>(127.1, 311.7))) * 43758.5453);
}
fn vnoise(p : vec2<f32>) -> f32 {
  let i = floor(p);
  let f = fract(p);
  let u = f * f * (3.0 - 2.0 * f);
  let a = hash22(i);
  let b = hash22(i + vec2<f32>(1.0, 0.0));
  let c = hash22(i + vec2<f32>(0.0, 1.0));
  let d = hash22(i + vec2<f32>(1.0, 1.0));
  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
fn fbm(p : vec2<f32>) -> f32 {
  var v = 0.0; var amp = 0.5; var fr = 1.0;
  for (var i = 0; i < 4; i = i + 1) { v = v + amp * vnoise(p * fr); fr = fr * 2.0; amp = amp * 0.5; }
  return v;
}
fn terrainHeight(x : i32, z : i32) -> i32 {
  let p = vec2<f32>(f32(x), f32(z)) * 0.013;
  return i32(44.0 + fbm(p) * 130.0 + fbm(p * 4.0) * 8.0); // broad foothills + fine
}
fn shade(r : i32, g : i32, b : i32, x : i32, y : i32, z : i32, ty : u32) -> u32 {
  let k = 0.86 + 0.22 * (f32(hash3(u32(x), u32(y), u32(z), 5u) & 0xFFFFu) / 65535.0);
  return (u32(clamp(i32(f32(r) * k), 0, 255)) << 24u)
       | (u32(clamp(i32(f32(g) * k), 0, 255)) << 16u)
       | (u32(clamp(i32(f32(b) * k), 0, 255)) << 8u) | ty;
}
fn ihash2(x : i32, z : i32) -> u32 {
  var h = u32(x) * 73856093u ^ u32(z) * 19349663u;
  h = (h ^ (h >> 15u)) * 2246822519u;
  return h ^ (h >> 13u);
}
fn treeAt(x : i32, y : i32, z : i32) -> u32 {
  let cell = 40;
  let canopyR = 9;
  let cgx = x / cell;
  let cgz = z / cell;
  for (var dz = -1; dz <= 1; dz = dz + 1) {
    for (var dx = -1; dx <= 1; dx = dx + 1) {
      let gx = cgx + dx;
      let gz = cgz + dz;
      let hh = ihash2(gx, gz);
      if ((hh & 1u) != 0u) { continue; }                       // ~half of cells
      let tx = gx * cell + 10 + i32(hh % 20u);
      let tz = gz * cell + 10 + i32((hh >> 10u) % 20u);
      let surf = terrainHeight(tx, tz);
      if (surf <= SEA + 4) { continue; }                       // grass only
      let topY = surf + 30 + i32((hh >> 20u) % 24u);
      if (abs(x - tx) <= 1 && abs(z - tz) <= 1 && y >= surf && y < topY) {
        return shade(101, 67, 33, x, y, z, 1u);                // trunk
      }
      let ddx = x - tx; let ddy = y - topY; let ddz = z - tz;
      if (ddx * ddx + ddy * ddy + ddz * ddz <= canopyR * canopyR) {
        return shade(54, 110, 48, x, y, z, 1u);                // canopy
      }
    }
  }
  return 0u;
}
fn sampleVoxel(x : i32, y : i32, z : i32) -> u32 {
  let h = terrainHeight(x, z);
  if (y < h) {
    let d = h - y;
    if (d <= 4) {
      if (h <= SEA + 4) { return shade(206, 192, 142, x, y, z, 1u); } // sand
      return shade(86, 168, 80, x, y, z, 1u);                        // grass
    }
    if (d <= 20) { return shade(122, 92, 60, x, y, z, 1u); }          // dirt
    return shade(108, 110, 124, x, y, z, 1u);                        // stone
  }
  if (y < SEA) {                                                     // lake water
    let dir = hash3(u32(x), u32(y), u32(z), 9u) & 3u;
    return shade(50, 110, 210, x, y, z, 2u | (dir << 2u));
  }
  if (y < h + 72) {                  // trees only live just above the surface
    let tree = treeAt(x, y, z);
    if (tree != 0u) { return tree; }
  }
  return 0u;
}

var<workgroup> occ : atomic<u32>;

@compute @workgroup_size(8, 8, 1)
fn generate(@builtin(local_invocation_id) lloc : vec3<u32>,
            @builtin(local_invocation_index) lidx : u32,
            @builtin(workgroup_id) wid : vec3<u32>) {
  let bi = u32(i32(wid.x) + i32(wid.y) * BG + i32(wid.z) * BG * BG);
  let lx = i32(lloc.x);
  let ly = i32(lloc.y);
  let bx = i32(wid.x) * 8 + lx;
  let by = i32(wid.y) * 8 + ly;
  let bz = i32(wid.z) * 8;
  if (lidx == 0u) { atomicStore(&occ, 0u); }
  workgroupBarrier();
  for (var lz = 0; lz < 8; lz = lz + 1) {
    let v = sampleVoxel(bx, by, bz + lz);
    let i = bi * 512u + u32(lx + ly * 8 + lz * 64);
    voxels0[i] = v;
    voxels1[i] = select(v, 0u, (v & 3u) == 2u); // static in both buffers; water only in buffer 0
    if (v != 0u) { atomicAdd(&occ, 1u); }
  }
  workgroupBarrier();
  if (lidx == 0u) {
    bricks[bi].occupancy = atomicLoad(&occ);
    bricks[bi].pointer = bi;
  }
}
