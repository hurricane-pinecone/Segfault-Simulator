// Click edit: ray-march to the first solid hit, then carve (mode 1) or spawn
// water (mode 2) a sphere there. One workgroup: thread 0 marches, all carve.
struct Edit { v0 : vec4<f32>, v1 : vec4<f32> }; // origin.xyz,radius ; dir.xyz,mode
@group(0) @binding(0) var<storage, read_write> voxCur : array<u32>;
@group(0) @binding(1) var<storage, read_write> voxOther : array<u32>;
@group(0) @binding(2) var<storage, read> bricks : array<Brick>;
@group(0) @binding(3) var<uniform> ed : Edit;

fn vIdx(x : i32, y : i32, z : i32) -> u32 {
  let bi = u32((x / 8) + (y / 8) * BG + (z / 8) * BG * BG);
  return bricks[bi].pointer * 512u + u32((x % 8) + (y % 8) * 8 + (z % 8) * 64);
}
fn inW(x : i32, y : i32, z : i32) -> bool { return x >= 0 && y >= 0 && z >= 0 && x < WG && y < WG && z < WG; }

var<workgroup> hit : vec3<i32>;
var<workgroup> pre : vec3<i32>;
var<workgroup> found : u32;

@compute @workgroup_size(64)
fn edit(@builtin(local_invocation_index) lid : u32) {
  if (lid == 0u) {
    found = 0u;
    let ro = ed.v0.xyz;
    let rd = normalize(ed.v1.xyz);
    let inv = vec3<f32>(1.0) / rd;
    let t0 = (vec3<f32>(0.0) - ro) * inv;
    let t1 = (vec3<f32>(f32(WG)) - ro) * inv;
    let tenter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z));
    let texit = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
    if (texit >= max(tenter, 0.0)) {
      let pos = ro + rd * (max(tenter, 0.0) + 0.0001);
      var voxel = vec3<i32>(floor(pos));
      let stp = vec3<i32>(sign(rd));
      let td = abs(inv);
      var tm = (vec3<f32>(f32(voxel.x) + select(0.0, 1.0, stp.x > 0),
                          f32(voxel.y) + select(0.0, 1.0, stp.y > 0),
                          f32(voxel.z) + select(0.0, 1.0, stp.z > 0)) - pos) * inv;
      var prev = voxel;
      for (var i = 0; i < 600; i = i + 1) {
        if (!inW(voxel.x, voxel.y, voxel.z)) { break; }
        if ((voxCur[vIdx(voxel.x, voxel.y, voxel.z)] & 3u) != 0u) { hit = voxel; pre = prev; found = 1u; break; }
        prev = voxel;
        if (tm.x < tm.y && tm.x < tm.z) { voxel.x += stp.x; tm.x += td.x; }
        else if (tm.y < tm.z) { voxel.y += stp.y; tm.y += td.y; }
        else { voxel.z += stp.z; tm.z += td.z; }
      }
    }
  }
  workgroupBarrier();
  if (found == 0u) { return; }
  let mode = u32(ed.v1.w);
  let R = i32(ed.v0.w);
  let center = select(hit, pre, mode == 2u);
  let dim = 2 * R + 1;
  let total = dim * dim * dim;
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
      voxCur[i] = 0u; voxOther[i] = 0u; // carve removes static from BOTH buffers
    } else if ((voxCur[i] & 3u) == 0u) {
      let dir = hash3(u32(wx), u32(wy), u32(wz), u32(idx)) & 3u;
      voxCur[i] = (50u << 24u) | (110u << 16u) | (210u << 8u) | 2u | (dir << 2u); // spawn water (current only)
    }
  }
}
