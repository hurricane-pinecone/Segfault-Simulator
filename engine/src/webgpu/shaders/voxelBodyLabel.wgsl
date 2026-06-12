// Voxel-level connected-component labeling of ONE rigid body's local grid (the
// one just carved), so a cut that disconnects it can be split. Pure CC: no
// ground anchor, no face masks -- two solid voxels are connected iff they are
// 6-adjacent. `slotU` selects which pool slot's grid to label. `recolor` is a
// debug pass that paints each component a distinct color in the body grid.
@group(0) @binding(1) var<storage, read> labelIn : array<u32>;
@group(0) @binding(2) var<storage, read_write> labelOut : array<u32>;
@group(0) @binding(3) var<uniform> slotU : vec4<u32>; // x = slot
// Sparse brickmap body voxels (read; recolor also overwrites existing cells).
@group(0) @binding(20) var<storage, read> bodyBrickGrid : array<u32>;
@group(0) @binding(21) var<storage, read_write> bodyBrickPool : array<u32>;
fn bodyVoxLoad(slot : u32, x : i32, y : i32, z : i32) -> u32 {
  let bp = bodyBrickGrid[slot * BODYBRICKS + brickCell(x, y, z)];
  if (bp == BRICK_EMPTY) { return 0u; }
  return bodyBrickPool[bp * 512u + brickLocal(x, y, z)];
}
fn bodyVoxSet(slot : u32, x : i32, y : i32, z : i32, v : u32) {
  let bp = bodyBrickGrid[slot * BODYBRICKS + brickCell(x, y, z)];
  if (bp == BRICK_EMPTY) { return; }
  bodyBrickPool[bp * 512u + brickLocal(x, y, z)] = v;
}

const DIM : i32 = BODYDIM;
const SENTINEL : u32 = 0xFFFFFFFFu;

fn lIdx(x : i32, y : i32, z : i32) -> u32 { return u32(x + y * DIM + z * DIM * DIM); }
fn solidAt(slot : u32, x : i32, y : i32, z : i32) -> bool {
  return (bodyVoxLoad(slot, x, y, z) & 3u) != 0u;
}

@compute @workgroup_size(4, 4, 4)
fn labelInit(@builtin(global_invocation_id) gid : vec3<u32>) {
  let x = i32(gid.x); let y = i32(gid.y); let z = i32(gid.z);
  if (x >= DIM || y >= DIM || z >= DIM) { return; }
  let li = lIdx(x, y, z);
  labelOut[li] = select(SENTINEL, li, solidAt(slotU.x, x, y, z));
}

fn nb(x : i32, y : i32, z : i32) -> u32 {
  if (x < 0 || y < 0 || z < 0 || x >= DIM || y >= DIM || z >= DIM) { return SENTINEL; }
  return labelIn[lIdx(x, y, z)]; // SENTINEL if that neighbor is air (max -> ignored by min)
}

@compute @workgroup_size(4, 4, 4)
fn labelFlood(@builtin(global_invocation_id) gid : vec3<u32>) {
  let x = i32(gid.x); let y = i32(gid.y); let z = i32(gid.z);
  if (x >= DIM || y >= DIM || z >= DIM) { return; }
  let li = lIdx(x, y, z);
  var lab = labelIn[li];
  if (lab == SENTINEL) { labelOut[li] = SENTINEL; return; }
  lab = min(lab, nb(x - 1, y, z));
  lab = min(lab, nb(x + 1, y, z));
  lab = min(lab, nb(x, y - 1, z));
  lab = min(lab, nb(x, y + 1, z));
  lab = min(lab, nb(x, y, z - 1));
  lab = min(lab, nb(x, y, z + 1));
  labelOut[li] = lab;
}

@compute @workgroup_size(4, 4, 4)
fn recolor(@builtin(global_invocation_id) gid : vec3<u32>) {
  let x = i32(gid.x); let y = i32(gid.y); let z = i32(gid.z);
  if (x >= DIM || y >= DIM || z >= DIM) { return; }
  let li = lIdx(x, y, z);
  let v = bodyVoxLoad(slotU.x, x, y, z);
  if ((v & 3u) == 0u) { return; }
  let h = labelIn[li] * 2654435761u;
  let r = (h >> 16u) & 255u;
  let g = (h >> 8u) & 255u;
  let b = h & 255u;
  bodyVoxSet(slotU.x, x, y, z, (r << 24u) | (g << 16u) | (b << 8u) | (v & 0xFFu));
}
