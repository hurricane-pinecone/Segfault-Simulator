// Per-slot reduce: each detached brick contributes its voxel AABB + brick count
// to its component's body slot (looked up via the label -> slot mapping). One
// brick per invocation. slotMeta is an array of MAXB slots, 16 i32 each:
// [0..2] aabbMin, [3..5] aabbMax, [6] brick count, [7..9] CoM-sum, [10] voxel
// count, [11..12] footprint-sum (x,z), [13] footprint count. Gate on solid faces
// (not occupancy) so water-only bricks are never treated as structural.
@group(0) @binding(0) var<storage, read> anchor : array<u32>;
@group(0) @binding(1) var<storage, read> faceBuf : array<u32>;
@group(0) @binding(2) var<storage, read> labelBuf : array<u32>;
@group(0) @binding(3) var<storage, read> rootSlot : array<u32>;
@group(0) @binding(4) var<storage, read_write> slotMeta : array<atomic<i32>>;

const SENTINEL : u32 = 0xFFFFFFFFu;

fn hasSolid(bi : u32) -> bool {
  var m = 0u;
  for (var k = 0u; k < 12u; k = k + 1u) { m = m | faceBuf[bi * 12u + k]; }
  return m != 0u;
}

@compute @workgroup_size(64)
fn reduce(@builtin(global_invocation_id) gid : vec3<u32>) {
  let bi = gid.x;
  if (bi >= u32(BG) * u32(BG) * u32(BG)) { return; }
  if (anchor[bi] != 0u || !hasSolid(bi)) { return; }
  let lab = labelBuf[bi];
  if (lab == SENTINEL) { return; }
  let slot = rootSlot[lab];
  if (slot >= MAXB) { return; }
  let base = slot * 16u;
  let bx = i32(bi % u32(BG));
  let by = i32((bi / u32(BG)) % u32(BG));
  let bz = i32(bi / (u32(BG) * u32(BG)));
  atomicMin(&slotMeta[base + 0u], bx * 8);
  atomicMin(&slotMeta[base + 1u], by * 8);
  atomicMin(&slotMeta[base + 2u], bz * 8);
  atomicMax(&slotMeta[base + 3u], bx * 8 + 8);
  atomicMax(&slotMeta[base + 4u], by * 8 + 8);
  atomicMax(&slotMeta[base + 5u], bz * 8 + 8);
  atomicAdd(&slotMeta[base + 6u], 1);
}
