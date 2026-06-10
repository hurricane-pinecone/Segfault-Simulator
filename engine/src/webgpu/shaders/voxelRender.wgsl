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
@group(0) @binding(4) var<storage, read> bodyVox : array<u32>;
@group(0) @binding(5) var<uniform> body : Body;

const LIGHT : vec3<f32> = vec3<f32>(0.4, 0.9, 0.3);

struct Hit {
  hit : bool,
  t : f32,
  col : vec3<f32>,
};

fn shade(v : u32, norm : vec3<f32>) -> vec3<f32> {
  let col = vec3<f32>(f32((v >> 24u) & 255u), f32((v >> 16u) & 255u), f32((v >> 8u) & 255u)) / 255.0;
  let diff = max(dot(norm, normalize(LIGHT)), 0.0) * 0.7 + 0.3;
  return col * diff;
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
          // Debug: a detached solid voxel glows red until it is extracted.
          if ((v & 3u) == 1u && (anchor[bidx] == 0u || (v & 0x10u) != 0u)) {
            h.col = vec3<f32>(1.0, 0.0, 0.0);
          } else {
            h.col = shade(v, vnorm);
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

fn marchBody(ro : vec3<f32>, rd : vec3<f32>) -> Hit {
  var h : Hit;
  h.hit = false;
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

  let tstart = max(tenter, 0.0) + 0.0001;
  let stepi = vec3<i32>(sign(rdl));
  let stepf = vec3<f32>(sign(rdl));
  let pos0 = max(stepf, vec3<f32>(0.0));
  let tDelta = abs(inv);

  var voxel = clamp(vec3<i32>(floor(rol + rdl * tstart)), vec3<i32>(0), vec3<i32>(dim - 1));
  var tMax = ((vec3<f32>(voxel) + pos0) - rol) * inv;
  var norm = vec3<f32>(0.0, 1.0, 0.0);
  var tVox = tstart;

  for (var i = 0; i < 220; i = i + 1) {
    if (voxel.x < 0 || voxel.y < 0 || voxel.z < 0 ||
        voxel.x >= dim || voxel.y >= dim || voxel.z >= dim) { break; }
    let v = bodyVox[u32(voxel.x + voxel.y * dim + voxel.z * dim * dim)];
    if ((v & 3u) != 0u) {
      h.hit = true;
      h.t = tVox;
      h.col = shade(v, norm);
      return h;
    }
    if (tMax.x < tMax.y && tMax.x < tMax.z) {
      tVox = tMax.x; voxel.x += stepi.x; tMax.x += tDelta.x; norm = vec3<f32>(-stepf.x, 0.0, 0.0);
    } else if (tMax.y < tMax.z) {
      tVox = tMax.y; voxel.y += stepi.y; tMax.y += tDelta.y; norm = vec3<f32>(0.0, -stepf.y, 0.0);
    } else {
      tVox = tMax.z; voxel.z += stepi.z; tMax.z += tDelta.z; norm = vec3<f32>(0.0, 0.0, -stepf.z);
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
  let b = marchBody(ro, rd);

  if (w.hit && b.hit) {
    if (b.t < w.t) { return vec4<f32>(b.col, 1.0); }
    return vec4<f32>(w.col, 1.0);
  }
  if (w.hit) { return vec4<f32>(w.col, 1.0); }
  if (b.hit) { return vec4<f32>(b.col, 1.0); }
  return vec4<f32>(bg, 1.0);
}
