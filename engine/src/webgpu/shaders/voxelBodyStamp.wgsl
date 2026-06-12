// Stamps each resting body (flags.z set by the CPU) back into the world at its
// final pose, turning the fallen chunk into terrain again. Brick-marched: one
// workgroup per body brick, empty bricks skipped. Stamped at the rest pose, which
// is a clean axis swap, so it leaves no holes.
struct Body {
  flags : vec4<u32>,
  invRot0 : vec4<f32>,
  invRot1 : vec4<f32>,
  invRot2 : vec4<f32>,
  center : vec4<f32>,
  pivot : vec4<f32>,
};
// Sparse brickmap body voxels (read-only).
@group(0) @binding(20) var<storage, read> bodyBrickGrid : array<u32>;
@group(0) @binding(21) var<storage, read> bodyBrickPool : array<u32>;
fn bodyVoxLoad(slot : u32, lx : i32, ly : i32, lz : i32) -> u32 {
  let bp = bodyBrickGrid[slot * BODYBRICKS + brickCell(lx, ly, lz)];
  if (bp == BRICK_EMPTY) { return 0u; }
  return bodyBrickPool[bp * 512u + brickLocal(lx, ly, lz)];
}
@group(0) @binding(1) var<storage, read_write> vox0 : array<u32>;
@group(0) @binding(2) var<storage, read_write> vox1 : array<u32>;
@group(0) @binding(3) var<storage, read> bodies : array<Body, MAXB>;

const SLOTVOX : u32 = BODYVOX;

fn vIndex(x : i32, y : i32, z : i32) -> u32 {
  let bi = (x / 8) + (y / 8) * BG + (z / 8) * BG * BG;
  return u32(bi) * 512u + u32((x % 8) + (y % 8) * 8 + (z % 8) * 64);
}

@compute @workgroup_size(64)
fn stamp(@builtin(workgroup_id) wid : vec3<u32>,
         @builtin(local_invocation_index) lidx : u32) {
  let slot = wid.z / u32(BODYBD);
  if (slot >= MAXB) { return; }
  let body = bodies[slot];
  if (body.flags.x == 0u || body.flags.z == 0u) { return; } // active + resting only
  let bz = wid.z % u32(BODYBD);
  let cell = wid.x + wid.y * u32(BODYBD) + bz * u32(BODYBD) * u32(BODYBD);
  let bp = bodyBrickGrid[slot * BODYBRICKS + cell];
  if (bp == BRICK_EMPTY) { return; }

  let R = mat3x3<f32>(body.invRot0.xyz, body.invRot1.xyz, body.invRot2.xyz);
  let Rlw = transpose(R);
  let cx = i32(lidx % 8u); let cy = i32(lidx / 8u);
  let bmx = i32(wid.x) * 8; let bmy = i32(wid.y) * 8; let bmz = i32(bz) * 8;
  for (var cz = 0; cz < 8; cz = cz + 1) {
    let v = bodyBrickPool[bp * 512u + u32(cx + cy * 8 + cz * 64)];
    if ((v & 3u) != 1u) { continue; }
    let L = vec3<f32>(f32(bmx + cx) + 0.5, f32(bmy + cy) + 0.5, f32(bmz + cz) + 0.5);
    let w = Rlw * (L - body.pivot.xyz) + body.center.xyz;
    let wi = vec3<i32>(floor(w));
    if (wi.x < 0 || wi.y < 0 || wi.z < 0 || wi.x >= WG || wi.y >= WG || wi.z >= WG) { continue; }
    let vi = vIndex(wi.x, wi.y, wi.z);
    vox0[vi] = v;
    vox1[vi] = v;
  }
}
