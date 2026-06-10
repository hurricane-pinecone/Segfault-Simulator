// Raymarched render: a fullscreen triangle, then a two-level DDA -- an outer
// brick DDA that skips empty bricks in one step, and an inner voxel DDA that ends
// on an integer bounds check (the voxel left the brick), not a fragile
// t-comparison. Both levels step integer coordinates, so the march always
// advances (no re-floor jumps that can stall).
struct Camera {
  p0 : vec4<f32>,  // camPos.xyz, worldSize
  p1 : vec4<f32>,  // rayForward.xyz, width
  p2 : vec4<f32>,  // rayRight.xyz, height
  p3 : vec4<f32>,  // rayUp.xyz, time
};
@group(0) @binding(0) var<uniform> cam : Camera;
@group(0) @binding(1) var<storage, read> voxels : array<u32>;
@group(0) @binding(2) var<storage, read> bricks : array<Brick>;
@group(0) @binding(3) var<storage, read> anchor : array<u32>;

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
  let inv = vec3<f32>(1.0) / rd;

  let t0 = (vec3<f32>(0.0) - ro) * inv;
  let t1 = (vec3<f32>(f32(WG)) - ro) * inv;
  let tenter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z));
  let texit = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
  if (texit < max(tenter, 0.0)) { return vec4<f32>(bg, 1.0); }

  let tstart = max(tenter, 0.0) + 0.0001;
  let lightDir = normalize(vec3<f32>(0.4, 0.9, 0.3));
  let stepi = vec3<i32>(sign(rd));
  let stepf = vec3<f32>(sign(rd));
  let pos0 = max(stepf, vec3<f32>(0.0)); // 1 where ray goes +, else 0 (next face offset)

  // --- Outer DDA over bricks; empty bricks are skipped in one step. ---
  // The box test guarantees the ray crosses the volume, so the entry brick is
  // valid -- but a ray grazing a face floors just outside (brick -1 or BG),
  // which would break on the first bounds check and dither the silhouette.
  // Clamp the seed brick to the edge so it marches inward instead.
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

      // --- Inner DDA over the brick's 8^3 voxels; ends on an integer bounds
      //     check (the voxel left the brick), not a fragile t-comparison. ---
      var voxel = clamp(vec3<i32>(floor(ro + rd * (tEnter + 0.0001))), bmin, bmin + vec3<i32>(7));
      var tMaxV = ((vec3<f32>(voxel) + pos0) - ro) * inv;
      var vnorm = bnorm;
      for (var iv = 0; iv < 26; iv = iv + 1) {
        let l = voxel - bmin;
        let v = voxels[slot * 512u + u32(l.x + l.y * 8 + l.z * 64)];
        if ((v & 3u) != 0u) {
          // Debug: a detached solid voxel glows red -- either its whole brick is
          // not ground-anchored (coarse), or the voxel refinement flagged it
          // (bit 4) at a carve boundary. After generation neither appears.
          if ((v & 3u) == 1u && (anchor[bidx] == 0u || (v & 0x10u) != 0u)) {
            return vec4<f32>(1.0, 0.0, 0.0, 1.0);
          }
          let col = vec3<f32>(f32((v >> 24u) & 255u), f32((v >> 16u) & 255u), f32((v >> 8u) & 255u)) / 255.0;
          let diff = max(dot(vnorm, lightDir), 0.0) * 0.7 + 0.3;
          return vec4<f32>(col * diff, 1.0);
        }
        if (tMaxV.x < tMaxV.y && tMaxV.x < tMaxV.z) {
          voxel.x += stepi.x; tMaxV.x += abs(inv.x); vnorm = vec3<f32>(-stepf.x, 0.0, 0.0);
          if (voxel.x < bmin.x || voxel.x > bmin.x + 7) { break; }
        } else if (tMaxV.y < tMaxV.z) {
          voxel.y += stepi.y; tMaxV.y += abs(inv.y); vnorm = vec3<f32>(0.0, -stepf.y, 0.0);
          if (voxel.y < bmin.y || voxel.y > bmin.y + 7) { break; }
        } else {
          voxel.z += stepi.z; tMaxV.z += abs(inv.z); vnorm = vec3<f32>(0.0, 0.0, -stepf.z);
          if (voxel.z < bmin.z || voxel.z > bmin.z + 7) { break; }
        }
      }
    }

    // Advance to the next brick.
    if (tMaxB.x < tMaxB.y && tMaxB.x < tMaxB.z) {
      brick.x += stepi.x; tEnter = tMaxB.x; tMaxB.x += tDeltaB.x; bnorm = vec3<f32>(-stepf.x, 0.0, 0.0);
    } else if (tMaxB.y < tMaxB.z) {
      brick.y += stepi.y; tEnter = tMaxB.y; tMaxB.y += tDeltaB.y; bnorm = vec3<f32>(0.0, -stepf.y, 0.0);
    } else {
      brick.z += stepi.z; tEnter = tMaxB.z; tMaxB.z += tDeltaB.z; bnorm = vec3<f32>(0.0, 0.0, -stepf.z);
    }
  }
  return vec4<f32>(bg, 1.0);
}
