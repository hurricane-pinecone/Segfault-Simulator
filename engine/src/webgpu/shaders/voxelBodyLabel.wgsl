// Connected-component labeling of ONE rigid body's local grid (the one just
// carved), so a cut that disconnects it can be split. Pure CC: no ground anchor,
// no face masks -- two solid voxels are connected iff they are 6-adjacent. `slotU`
// selects which pool slot's grid to label. Node-resolution: a within-brick local
// CC seeds the node graph, the flood equalizes it, then a scatter writes labels.
@group(0) @binding(2) var<storage, read_write> labelOut : array<u32>;
@group(0) @binding(3) var<uniform> slotU : vec4<u32>; // x = slot
// Sparse brickmap body voxels (read).
@group(0) @binding(20) var<storage, read> bodyBrickGrid : array<u32>;
@group(0) @binding(21) var<storage, read_write> bodyBrickPool : array<u32>;
// Two-level CC (mirrors the world fell). bodyLocalCC labels each solid voxel's
// within-brick component into voxLocalLabel; nodeLabel holds the min label per
// (brick, component) so a component equalizes in ~one step, while a cut keeps its
// two sides as separate components -> a carved-off chunk gets its own label/body.
@group(0) @binding(4) var<storage, read_write> voxLocalLabel : array<u32>;
@group(0) @binding(5) var<storage, read_write> nodeLabel : array<atomic<u32>>;
// Per body-brick (12^3) flag: 1 if the brick holds any solid voxel. The node
// flood dispatches one workgroup per brick and skips the empty ones.
@group(0) @binding(6) var<storage, read_write> detBrick : array<u32>;
fn bodyVoxLoad(slot : u32, x : i32, y : i32, z : i32) -> u32 {
  let bp = bodyBrickGrid[slot * BODYBRICKS + brickCell(x, y, z)];
  if (bp == BRICK_EMPTY) { return 0u; }
  return bodyBrickPool[bp * 512u + brickLocal(x, y, z)];
}

const DIM : i32 = BODYDIM;
const SENTINEL : u32 = 0xFFFFFFFFu;

fn lIdx(x : i32, y : i32, z : i32) -> u32 { return u32(x + y * DIM + z * DIM * DIM); }
fn solidAt(slot : u32, x : i32, y : i32, z : i32) -> bool {
  return (bodyVoxLoad(slot, x, y, z) & 3u) != 0u;
}

// Within-brick CC of slotU.x's grid (one workgroup per body-brick, a z-column per
// thread, shared memory). Publishes each voxel's within-brick component label,
// seeds each (brick, component) node with its component's min global index for the
// node flood, and records whether the brick holds any solid voxel.
var<workgroup> lblB : array<u32, 512>;
var<workgroup> wgDet : atomic<u32>;
@compute @workgroup_size(64)
fn bodyLocalCC(@builtin(workgroup_id) wid : vec3<u32>,
               @builtin(local_invocation_index) lidx : u32) {
  let slot = slotU.x;
  let bx = i32(wid.x) * 8; let by = i32(wid.y) * 8; let bz = i32(wid.z) * 8;
  let brick = wid.x + wid.y * 12u + wid.z * 144u;
  let nodeBase = brick * 512u;
  let cx = i32(lidx % 8u); let cy = i32(lidx / 8u);
  if (lidx == 0u) { atomicStore(&wgDet, 0u); }
  for (var z = 0; z < 8; z = z + 1) {
    let li = u32(cx + cy * 8 + z * 64);
    lblB[li] = select(SENTINEL, li, solidAt(slot, bx + cx, by + cy, bz + z));
    atomicStore(&nodeLabel[nodeBase + li], SENTINEL);
  }
  workgroupBarrier();
  for (var it = 0; it < 24; it = it + 1) {
    for (var z = 0; z < 8; z = z + 1) {
      let li = u32(cx + cy * 8 + z * 64);
      var m = lblB[li];
      if (m != SENTINEL) {
        if (cx > 0) { m = min(m, lblB[li - 1u]); }
        if (cx < 7) { m = min(m, lblB[li + 1u]); }
        if (cy > 0) { m = min(m, lblB[li - 8u]); }
        if (cy < 7) { m = min(m, lblB[li + 8u]); }
        if (z > 0)  { m = min(m, lblB[li - 64u]); }
        if (z < 7)  { m = min(m, lblB[li + 64u]); }
        lblB[li] = m;
      }
    }
    workgroupBarrier();
  }
  for (var z = 0; z < 8; z = z + 1) {
    let li = u32(cx + cy * 8 + z * 64);
    let comp = lblB[li];
    voxLocalLabel[lIdx(bx + cx, by + cy, bz + z)] = comp;
    if (comp != SENTINEL) {
      atomicMin(&nodeLabel[nodeBase + comp], lIdx(bx + cx, by + cy, bz + z));
      atomicStore(&wgDet, 1u);
    }
  }
  workgroupBarrier();
  if (lidx == 0u) { detBrick[brick] = atomicLoad(&wgDet); }
}

// Node flood: one workgroup per body-brick (12^3), 64 threads scan the brick's 6
// faces and equalize face-adjacent (brick, component) node mins; empty bricks
// skip. Re-dispatched ~brick-diameter times; in-place atomicMin converges.
fn conduct(brick : u32, wx : i32, wy : i32, wz : i32, ox : i32, oy : i32, oz : i32) {
  let comp = voxLocalLabel[lIdx(wx, wy, wz)];
  if (comp == SENTINEL) { return; }
  let nwx = wx + ox; let nwy = wy + oy; let nwz = wz + oz;
  if (nwx < 0 || nwy < 0 || nwz < 0 || nwx >= DIM || nwy >= DIM || nwz >= DIM) { return; }
  let nComp = voxLocalLabel[lIdx(nwx, nwy, nwz)];
  if (nComp == SENTINEL) { return; }
  let nBrick = u32((nwx / 8) + (nwy / 8) * 12 + (nwz / 8) * 144);
  let myNode = brick * 512u + comp;
  let nNode = nBrick * 512u + nComp;
  let m = min(atomicLoad(&nodeLabel[myNode]), atomicLoad(&nodeLabel[nNode]));
  atomicMin(&nodeLabel[myNode], m);
  atomicMin(&nodeLabel[nNode], m);
}
@compute @workgroup_size(64)
fn nodeFlood(@builtin(workgroup_id) wid : vec3<u32>,
             @builtin(local_invocation_index) lidx : u32) {
  let brick = wid.x + wid.y * 12u + wid.z * 144u;
  if (detBrick[brick] == 0u) { return; }
  let bx = i32(wid.x) * 8; let by = i32(wid.y) * 8; let bz = i32(wid.z) * 8;
  let a = i32(lidx / 8u); let b = i32(lidx % 8u);
  conduct(brick, bx + 0, by + a, bz + b, -1, 0, 0);
  conduct(brick, bx + 7, by + a, bz + b, 1, 0, 0);
  conduct(brick, bx + a, by + 0, bz + b, 0, -1, 0);
  conduct(brick, bx + a, by + 7, bz + b, 0, 1, 0);
  conduct(brick, bx + a, by + b, bz + 0, 0, 0, -1);
  conduct(brick, bx + a, by + b, bz + 7, 0, 0, 1);
}

// Scatter each solid voxel's final node label back to labelOut (the buffer the
// split register/reduce read); air -> SENTINEL.
@compute @workgroup_size(4, 4, 4)
fn scatter(@builtin(global_invocation_id) gid : vec3<u32>) {
  let x = i32(gid.x); let y = i32(gid.y); let z = i32(gid.z);
  if (x >= DIM || y >= DIM || z >= DIM) { return; }
  let li = lIdx(x, y, z);
  let comp = voxLocalLabel[li];
  if (comp == SENTINEL) { labelOut[li] = SENTINEL; return; }
  labelOut[li] = atomicLoad(&nodeLabel[brickCell(x, y, z) * 512u + comp]);
}

