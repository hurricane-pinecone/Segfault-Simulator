// Raymarched render. marchWorld() walks the static brickmap (a two-level DDA --
// outer brick DDA skipping empty bricks, inner voxel DDA ending on an integer
// bounds check). marchBody() walks a detached rigid body's small local voxel
// grid, transformed into body space. fs_main composites them by depth, so a
// moving solid voxel object occludes / is occluded by the world correctly.
struct Camera {
  p0 : vec4<f32>,  // camPos.xyz, worldSize
  p1 : vec4<f32>,  // rayForward.xyz, width
  p2 : vec4<f32>,  // rayRight.xyz, height
  p3 : vec4<f32>,  // rayUp.xyz, time
};
// A rigid body: a BODY_DIM^3 voxel grid in body-local space, placed by a rotation
// (stored inverse, world->local) about a pivot. active 0 = no body.
struct Body {
  flags : vec4<u32>,   // x active, y dim
  invRot0 : vec4<f32>, // inverse-rotation columns (world -> local), xyz used
  invRot1 : vec4<f32>,
  invRot2 : vec4<f32>,
  center : vec4<f32>,  // pivot position in the world (xyz)
  pivot : vec4<f32>,   // pivot in body-local coords (xyz)
};
@group(0) @binding(0) var<uniform> cam : Camera;
@group(0) @binding(1) var<storage, read> voxels : array<u32>;
@group(0) @binding(2) var<storage, read> bricks : array<Brick>;
@group(0) @binding(3) var<storage, read> anchor : array<u32>;
// body voxels now come from the sparse brickmap (bodyVoxLoad, bindings 20/21).
@group(0) @binding(5) var<storage, read> bodies : array<Body, MAXB>;
@group(0) @binding(6) var<storage, read> labelBuf : array<u32>;
@group(0) @binding(7) var<uniform> dbgMouse : vec4<f32>; // xy = cursor pixel
@group(0) @binding(8) var<storage, read> materials : array<Material>;
@group(0) @binding(9) var<storage, read> bodyArgs : array<u32>; // [3] = active body bound
// Sparse brickmap body voxels (read-only); marchBody DDAs over these directly.
@group(0) @binding(20) var<storage, read> bodyBrickGrid : array<u32>;
@group(0) @binding(21) var<storage, read> bodyBrickPool : array<u32>;

const SLOTVOX : u32 = BODYVOX; // body grid DIM^3 (per pool slot)

const LIGHT : vec3<f32> = vec3<f32>(0.4, 0.9, 0.3);

// Distinct color per connected-component label (debug viz of the detached set).
fn hashColor(lbl : u32) -> vec3<f32> {
  let h = lbl * 2654435761u;
  return vec3<f32>(f32((h >> 16u) & 255u),
                   f32((h >> 8u) & 255u),
                   f32(h & 255u)) / 255.0 * 0.85 + 0.15;
}

struct Hit {
  hit : bool,
  t : f32,
  col : vec3<f32>,
};

// Material colour from the palette, with a per-voxel brightness jitter (keyed on
// the voxel coordinate -- moved here from the generator now that colour is not
// baked per voxel) for texture, then a simple directional shade.
fn shade(v : u32, norm : vec3<f32>, vc : vec3<i32>) -> vec3<f32> {
  let k = 0.86 + 0.22 * (f32(hash3(u32(vc.x), u32(vc.y), u32(vc.z), 5u) & 0xFFFFu) / 65535.0);
  let col = materials[matId(v)].color.rgb * k;
  let diff = max(dot(norm, normalize(LIGHT)), 0.0) * 0.7 + 0.3;
  return col * diff;
}

// Debug overlay: true on a thin line where the hit surface crosses an 8^3 brick
// boundary plane. Only the two IN-PLANE axes are tested (the normal axis is
// constant on the face, so a face co-planar with a brick boundary doesn't flood).
// p is the hit point (in the same space as the brick grid: world or body-local).
fn brickWire(p : vec3<f32>, n : vec3<f32>) -> bool {
  let lw = 0.05; // line half-width in voxels
  let d = abs(p - round(p / 8.0) * 8.0); // distance to nearest brick plane per axis
  return (abs(n.x) < 0.5 && d.x < lw) ||
         (abs(n.y) < 0.5 && d.y < lw) ||
         (abs(n.z) < 0.5 && d.z < lw);
}

fn marchWorld(ro : vec3<f32>, rd : vec3<f32>) -> Hit {
  var h : Hit;
  h.hit = false;
  let inv = vec3<f32>(1.0) / rd;

  let t0 = (vec3<f32>(0.0) - ro) * inv;
  let t1 = (vec3<f32>(f32(WG)) - ro) * inv;
  let tenter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z));
  let texit = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
  if (texit < max(tenter, 0.0)) { return h; }

  let tstart = max(tenter, 0.0) + 0.0001;
  let stepi = vec3<i32>(sign(rd));
  let stepf = vec3<f32>(sign(rd));
  let pos0 = max(stepf, vec3<f32>(0.0));

  // Clamp the seed brick to the edge so a grazing ray marches inward (else it
  // breaks on the first bounds check and dithers the silhouette).
  var brick = clamp(vec3<i32>(floor((ro + rd * tstart) / 8.0)),
                    vec3<i32>(0), vec3<i32>(BG - 1));
  var tMaxB = ((vec3<f32>(brick) + pos0) * 8.0 - ro) * inv;
  let tDeltaB = abs(inv) * 8.0;
  var tEnter = tstart;
  var bnorm = vec3<f32>(0.0, 1.0, 0.0);

  for (var ob = 0; ob < 210; ob = ob + 1) {
    if (brick.x < 0 || brick.y < 0 || brick.z < 0 || brick.x >= BG || brick.y >= BG || brick.z >= BG) { break; }
    let bidx = u32(brick.x + brick.y * BG + brick.z * BG * BG);
    if (bricks[bidx].occupancy != 0u) {
      let slot = bricks[bidx].pointer;
      let bmin = brick * 8;

      var voxel = clamp(vec3<i32>(floor(ro + rd * (tEnter + 0.0001))), bmin, bmin + vec3<i32>(7));
      var tMaxV = ((vec3<f32>(voxel) + pos0) - ro) * inv;
      var vnorm = bnorm;
      var tVox = tEnter;
      for (var iv = 0; iv < 26; iv = iv + 1) {
        let l = voxel - bmin;
        let v = voxels[slot * 512u + u32(l.x + l.y * 8 + l.z * 64)];
        if ((v & 3u) != 0u) {
          h.hit = true;
          h.t = tVox;
          // Debug tint: brick-detached (anchor 0) solid is hash-coloured to flag
          // floating/ungrounded geometry -- e.g. a toppled body stamped into the
          // air. Normal grounded voxels shade normally.
          if ((v & 3u) == 1u && (v & 0x10u) == 0u && anchor[bidx] == 0u) {
            h.col = hashColor(labelBuf[bidx]);
          } else {
            h.col = shade(v, vnorm, voxel);
          }
          if (dbgMouse.w != 0.0 && brickWire(ro + rd * tVox, vnorm)) {
            h.col = vec3<f32>(0.0, 1.0, 1.0);
          }
          return h;
        }
        if (tMaxV.x < tMaxV.y && tMaxV.x < tMaxV.z) {
          tVox = tMaxV.x; voxel.x += stepi.x; tMaxV.x += abs(inv.x); vnorm = vec3<f32>(-stepf.x, 0.0, 0.0);
          if (voxel.x < bmin.x || voxel.x > bmin.x + 7) { break; }
        } else if (tMaxV.y < tMaxV.z) {
          tVox = tMaxV.y; voxel.y += stepi.y; tMaxV.y += abs(inv.y); vnorm = vec3<f32>(0.0, -stepf.y, 0.0);
          if (voxel.y < bmin.y || voxel.y > bmin.y + 7) { break; }
        } else {
          tVox = tMaxV.z; voxel.z += stepi.z; tMaxV.z += abs(inv.z); vnorm = vec3<f32>(0.0, 0.0, -stepf.z);
          if (voxel.z < bmin.z || voxel.z > bmin.z + 7) { break; }
        }
      }
    }

    if (tMaxB.x < tMaxB.y && tMaxB.x < tMaxB.z) {
      brick.x += stepi.x; tEnter = tMaxB.x; tMaxB.x += tDeltaB.x; bnorm = vec3<f32>(-stepf.x, 0.0, 0.0);
    } else if (tMaxB.y < tMaxB.z) {
      brick.y += stepi.y; tEnter = tMaxB.y; tMaxB.y += tDeltaB.y; bnorm = vec3<f32>(0.0, -stepf.y, 0.0);
    } else {
      brick.z += stepi.z; tEnter = tMaxB.z; tMaxB.z += tDeltaB.z; bnorm = vec3<f32>(0.0, 0.0, -stepf.z);
    }
  }
  return h;
}

fn marchBody(ro : vec3<f32>, rd : vec3<f32>, slot : u32) -> Hit {
  var h : Hit;
  h.hit = false;
  let body = bodies[slot];
  if (body.flags.x == 0u) { return h; }
  let dim = i32(body.flags.y);

  // World ray -> body-local: rotate about the pivot (invRot is world->local).
  let R = mat3x3<f32>(body.invRot0.xyz, body.invRot1.xyz, body.invRot2.xyz);
  let rol = R * (ro - body.center.xyz) + body.pivot.xyz;
  let rdl = R * rd; // orthonormal R -> |rdl| = 1, so local t == world t
  let inv = vec3<f32>(1.0) / rdl;

  let t0 = (vec3<f32>(0.0) - rol) * inv;
  let t1 = (vec3<f32>(f32(dim)) - rol) * inv;
  let tenter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z));
  let texit = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
  if (texit < max(tenter, 0.0)) { return h; }

  // Two-level DDA over the body's sparse brickmap: the outer brick DDA skips
  // EMPTY (unallocated) bricks wholesale -- most of a 96^3 grid -- while an
  // occupied brick is still marched voxel-by-voxel, so single-voxel hits (and the
  // carve's single-voxel cuts) stay exact. Mirrors marchWorld at body scale; the
  // brick pointer is read once per brick, not per voxel.
  let tstart = max(tenter, 0.0) + 0.0001;
  let stepi = vec3<i32>(sign(rdl));
  let stepf = vec3<f32>(sign(rdl));
  let pos0 = max(stepf, vec3<f32>(0.0));
  let g0 = slot * BODYBRICKS;

  var brick = clamp(vec3<i32>(floor((rol + rdl * tstart) / 8.0)),
                    vec3<i32>(0), vec3<i32>(BODYBD - 1));
  var tMaxB = ((vec3<f32>(brick) + pos0) * 8.0 - rol) * inv;
  let tDeltaB = abs(inv) * 8.0;
  var tEnter = tstart;
  var bnorm = vec3<f32>(0.0, 1.0, 0.0);

  for (var ob = 0; ob < 40; ob = ob + 1) {
    if (brick.x < 0 || brick.y < 0 || brick.z < 0 ||
        brick.x >= BODYBD || brick.y >= BODYBD || brick.z >= BODYBD) { break; }
    let bp = bodyBrickGrid[g0 + u32(brick.x + brick.y * BODYBD + brick.z * BODYBD * BODYBD)];
    if (bp != BRICK_EMPTY) {
      let bmin = brick * 8;
      var voxel = clamp(vec3<i32>(floor(rol + rdl * (tEnter + 0.0001))), bmin, bmin + vec3<i32>(7));
      var tMaxV = ((vec3<f32>(voxel) + pos0) - rol) * inv;
      var vnorm = bnorm;
      var tVox = tEnter;
      for (var iv = 0; iv < 26; iv = iv + 1) {
        let l = voxel - bmin;
        let v = bodyBrickPool[bp * 512u + u32(l.x + l.y * 8 + l.z * 64)];
        if ((v & 3u) != 0u) {
          h.hit = true;
          h.t = tVox;
          h.col = shade(v, vnorm, voxel);
          // Body-local brick grid (8^3); wireframe in body-local space.
          if (dbgMouse.w != 0.0 && brickWire(rol + rdl * tVox, vnorm)) {
            h.col = vec3<f32>(0.0, 1.0, 1.0);
          }
          return h;
        }
        if (tMaxV.x < tMaxV.y && tMaxV.x < tMaxV.z) {
          tVox = tMaxV.x; voxel.x += stepi.x; tMaxV.x += abs(inv.x); vnorm = vec3<f32>(-stepf.x, 0.0, 0.0);
          if (voxel.x < bmin.x || voxel.x > bmin.x + 7) { break; }
        } else if (tMaxV.y < tMaxV.z) {
          tVox = tMaxV.y; voxel.y += stepi.y; tMaxV.y += abs(inv.y); vnorm = vec3<f32>(0.0, -stepf.y, 0.0);
          if (voxel.y < bmin.y || voxel.y > bmin.y + 7) { break; }
        } else {
          tVox = tMaxV.z; voxel.z += stepi.z; tMaxV.z += abs(inv.z); vnorm = vec3<f32>(0.0, 0.0, -stepf.z);
          if (voxel.z < bmin.z || voxel.z > bmin.z + 7) { break; }
        }
      }
    }
    if (tMaxB.x < tMaxB.y && tMaxB.x < tMaxB.z) {
      brick.x += stepi.x; tEnter = tMaxB.x; tMaxB.x += tDeltaB.x; bnorm = vec3<f32>(-stepf.x, 0.0, 0.0);
    } else if (tMaxB.y < tMaxB.z) {
      brick.y += stepi.y; tEnter = tMaxB.y; tMaxB.y += tDeltaB.y; bnorm = vec3<f32>(0.0, -stepf.y, 0.0);
    } else {
      brick.z += stepi.z; tEnter = tMaxB.z; tMaxB.z += tDeltaB.z; bnorm = vec3<f32>(0.0, 0.0, -stepf.z);
    }
  }

  // Debug overlay: a magenta wireframe of the body's occupied-brick AABB (packed
  // by the xform pass into invRot0.w / invRot1.w) -- the extent the march actually
  // covers, not the full BODYDIM^3 grid. Reached only when the ray hit NO voxel,
  // so it frames the body without occluding it. A box face point is on an edge
  // when it sits near two of the six AABB faces; check the entry + exit faces.
  if (dbgMouse.w != 0.0) {
    let pLo = bitcast<u32>(body.invRot0.w);
    let pHi = bitcast<u32>(body.invRot1.w);
    let lo = vec3<f32>(f32(pLo & 0xFFu), f32((pLo >> 8u) & 0xFFu), f32((pLo >> 16u) & 0xFFu));
    let hi = vec3<f32>(f32(pHi & 0xFFu), f32((pHi >> 8u) & 0xFFu), f32((pHi >> 16u) & 0xFFu));
    if (all(hi > lo)) {
      let ta = (lo - rol) * inv;
      let tb = (hi - rol) * inv;
      let tn = max(max(min(ta.x, tb.x), min(ta.y, tb.y)), min(ta.z, tb.z));
      let tx = min(min(max(ta.x, tb.x), max(ta.y, tb.y)), max(ta.z, tb.z));
      if (tx >= max(tn, 0.0)) {
        let ew = 1.5;
        let pe = rol + rdl * max(tn, 0.0);
        let px = rol + rdl * tx;
        var ne = 0;
        if (abs(pe.x - lo.x) < ew || abs(pe.x - hi.x) < ew) { ne = ne + 1; }
        if (abs(pe.y - lo.y) < ew || abs(pe.y - hi.y) < ew) { ne = ne + 1; }
        if (abs(pe.z - lo.z) < ew || abs(pe.z - hi.z) < ew) { ne = ne + 1; }
        var nx = 0;
        if (abs(px.x - lo.x) < ew || abs(px.x - hi.x) < ew) { nx = nx + 1; }
        if (abs(px.y - lo.y) < ew || abs(px.y - hi.y) < ew) { nx = nx + 1; }
        if (abs(px.z - lo.z) < ew || abs(px.z - hi.z) < ew) { nx = nx + 1; }
        if (ne >= 2 || nx >= 2) {
          h.hit = true;
          h.t = max(tn, 0.0);
          h.col = vec3<f32>(1.0, 0.0, 1.0);
        }
      }
    }
  }
  return h;
}

@vertex
fn vs_main(@builtin(vertex_index) vid : u32) -> @builtin(position) vec4<f32> {
  var p = array<vec2<f32>, 3>(vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(p[vid], 0.0, 1.0);
}

@fragment
fn fs_main(@builtin(position) fragPos : vec4<f32>) -> @location(0) vec4<f32> {
  let res = vec2<f32>(cam.p1.w, cam.p2.w);
  let ndc = vec2<f32>(fragPos.x / res.x * 2.0 - 1.0, 1.0 - fragPos.y / res.y * 2.0);
  let bg = mix(vec3<f32>(0.10, 0.12, 0.18), vec3<f32>(0.02, 0.02, 0.04), ndc.y * 0.5 + 0.5);

  let ro = cam.p0.xyz;
  let rd = normalize(cam.p1.xyz + ndc.x * cam.p2.xyz + ndc.y * cam.p3.xyz);

  let w = marchWorld(ro, rd);

  // Nearest hit across the body pool, then depth-merge with the world.
  var b : Hit;
  b.hit = false;
  b.t = 1.0e30;
  // Only the live slots (bodyArgs[3] = highest active slot + 1, written by the
  // prep pass) -- empty slots cost nothing, so the pool cap doesn't tax every
  // pixel. Read from a storage buffer, not the dbgMouse uniform: a compute
  // storage write feeding a uniform read of the same buffer is unreliable across
  // backends and silently leaves the bound at 0 (every body invisible).
  let bound = bodyArgs[3];
  for (var s = 0u; s < bound; s = s + 1u) {
    let bs = marchBody(ro, rd, s);
    if (bs.hit && bs.t < b.t) { b = bs; }
  }

  // Debug overlay: a box at the cursor shows what its ray hits -- green = a
  // rigid body, red = world, blue = nothing.
  if (abs(fragPos.x - dbgMouse.x) < 3.0 && abs(fragPos.y - dbgMouse.y) < 3.0) {
    if (b.hit && (!w.hit || b.t < w.t)) { return vec4<f32>(0.0, 1.0, 0.0, 1.0); }
    if (w.hit) { return vec4<f32>(1.0, 0.0, 0.0, 1.0); }
    return vec4<f32>(0.2, 0.2, 1.0, 1.0);
  }

  if (w.hit && b.hit) {
    if (b.t < w.t) { return vec4<f32>(b.col, 1.0); }
    return vec4<f32>(w.col, 1.0);
  }
  if (w.hit) { return vec4<f32>(w.col, 1.0); }
  if (b.hit) { return vec4<f32>(b.col, 1.0); }
  return vec4<f32>(bg, 1.0);
}
