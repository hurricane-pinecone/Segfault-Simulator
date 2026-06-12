// Break-off: when the step records a hard impact (magnitude + contact point in
// state slot 6), body voxels NEAR that contact shed into the world as rubble,
// with a per-voxel chance scaled by impact / material rigidity -- so loose
// materials crumble on impact while rigid ones mostly hold. A shed voxel keeps
// its material colour but becomes a powder (falls + piles via the fluid CA) and
// is removed from the body. Brick-marched: one workgroup per body brick, empty
// bricks skipped.
//
// Writes the dynamic rubble into the CURRENT src buffer only (voxCur); writing
// both buffers would duplicate it through the ping-pong.
struct Body {
  flags : vec4<u32>,
  invRot0 : vec4<f32>,
  invRot1 : vec4<f32>,
  invRot2 : vec4<f32>,
  center : vec4<f32>,
  pivot : vec4<f32>,
};
// Sparse brickmap body voxels: read + clear (the shed only removes existing solid
// voxels, so no allocation is needed).
@group(0) @binding(20) var<storage, read> bodyBrickGrid : array<u32>;
@group(0) @binding(21) var<storage, read_write> bodyBrickPool : array<u32>;
fn bodyVoxLoad(slot : u32, lx : i32, ly : i32, lz : i32) -> u32 {
  let bp = bodyBrickGrid[slot * BODYBRICKS + brickCell(lx, ly, lz)];
  if (bp == BRICK_EMPTY) { return 0u; }
  return bodyBrickPool[bp * 512u + brickLocal(lx, ly, lz)];
}
fn bodyVoxClear(slot : u32, lx : i32, ly : i32, lz : i32) {
  let bp = bodyBrickGrid[slot * BODYBRICKS + brickCell(lx, ly, lz)];
  if (bp == BRICK_EMPTY) { return; }
  bodyBrickPool[bp * 512u + brickLocal(lx, ly, lz)] = 0u;
}
@group(0) @binding(1) var<storage, read_write> voxCur : array<u32>;
@group(0) @binding(2) var<storage, read> bodies : array<Body, MAXB>;
@group(0) @binding(3) var<storage, read> state : array<vec4<f32>>;
@group(0) @binding(4) var<storage, read> materials : array<Material>;
@group(0) @binding(5) var<uniform> frame : vec4<u32>;

const SLOTVOX : u32 = BODYVOX;
const SHED_FALLOFF : f32 = 60.0; // chance falls off with distance from the contact
const SHED_K : f32 = 0.01;       // chance = impact * falloff * K / rigidity

fn vIndex(x : i32, y : i32, z : i32) -> u32 {
  let bi = (x / 8) + (y / 8) * BG + (z / 8) * BG * BG;
  return u32(bi) * 512u + u32((x % 8) + (y % 8) * 8 + (z % 8) * 64);
}

@compute @workgroup_size(64)
fn shed(@builtin(workgroup_id) wid : vec3<u32>,
        @builtin(local_invocation_index) lidx : u32) {
  let slot = wid.z / u32(BODYBD);
  if (slot >= MAXB) { return; }
  let imp = state[slot * 8u + 6u]; // (impactMag, contact.xyz)
  if (imp.x <= 0.0) { return; }     // no hard impact this body this frame
  let body = bodies[slot];
  if (body.flags.x == 0u) { return; } // active only
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
    if ((v & 3u) != 1u) { continue; } // solid body voxel
    let lx = bmx + cx; let ly = bmy + cy; let lz = bmz + cz;
    let L = vec3<f32>(f32(lx) + 0.5, f32(ly) + 0.5, f32(lz) + 0.5);
    let w = Rlw * (L - body.pivot.xyz) + body.center.xyz;

    // The whole rigid body feels the impact (it decelerates as one); the contact
    // patch crumbles most, but the shock carries through the body with a soft
    // falloff -- so a trunk-first landing still shakes leaves loose up top, and a
    // bigger impulse reaches further. No hard cutoff.
    let dist = length(w - imp.yzw);
    let falloff = SHED_FALLOFF / (SHED_FALLOFF + dist);
    let rig = max(materials[matId(v)].rigidity, 0.05);
    let chance = imp.x * SHED_K * falloff / rig;
    let wi = vec3<i32>(floor(w));
    let rnd = f32(hash3(u32(wi.x), u32(wi.y), u32(wi.z), frame.x) & 0xFFFFu) / 65535.0;
    if (rnd >= chance) { continue; }

    if (wi.x < 0 || wi.y < 0 || wi.z < 0 || wi.x >= WG || wi.y >= WG || wi.z >= WG) { continue; }
    let vi = vIndex(wi.x, wi.y, wi.z);
    if ((voxCur[vi] & 3u) != 0u) { continue; } // shed only into an air cell
    voxCur[vi] = vox(matId(v), CAT_LIQUID) | VOX_POWDER; // rubble keeps its colour
    bodyVoxClear(slot, lx, ly, lz);                      // remove from the body
  }
}
