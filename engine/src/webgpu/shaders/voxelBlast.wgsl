// Explosion blast. Thread 0 ray-marches to the cursor's first solid world hit,
// publishes the blast (center / radius / force) for the downstream passes (body
// impulse, and later the ballistic-debris spawn), and seeds carveHit so the world
// fell centres its window on the crater. All 64 threads then carve the crater
// sphere. A dedicated pass (not an edit mode): explosions are a game mechanic, and
// this is where the debris-shell ejection will live.
struct BlastIn { v0 : vec4<f32>, v1 : vec4<f32> }; // origin.xyz,radius ; dir.xyz,force
@group(0) @binding(0) var<storage, read_write> voxCur : array<u32>;
@group(0) @binding(1) var<storage, read_write> voxOther : array<u32>;
@group(0) @binding(2) var<storage, read> bricks : array<Brick>;
@group(0) @binding(3) var<uniform> bi : BlastIn;
@group(0) @binding(4) var<storage, read_write> dirty : array<atomic<u32>>;
@group(0) @binding(7) var<storage, read_write> carveHit : array<u32>;
// Published blast: [0..2]=center, [3]=radius, [4]=force, [5]=active (0 = ray
// missed, downstream passes no-op).
@group(0) @binding(8) var<storage, read_write> blast : array<f32>;
// Ballistic debris pool + its ring-allocation head. Solid crater voxels are
// ejected as outward-flying debris (the advect pass collides + settles them).
@group(0) @binding(9) var<storage, read_write> debris : array<Debris>;
@group(0) @binding(10) var<storage, read_write> debrisHead : array<atomic<u32>>;

fn vIdx(x : i32, y : i32, z : i32) -> u32 {
  let bi2 = u32((x / 8) + (y / 8) * BG + (z / 8) * BG * BG);
  return bricks[bi2].pointer * 512u + u32((x % 8) + (y % 8) * 8 + (z % 8) * 64);
}
fn inW(x : i32, y : i32, z : i32) -> bool {
  return x >= 0 && y >= 0 && z >= 0 && x < WG && y < WG && z < WG;
}

struct March { found : bool, voxel : vec3<i32>, t : f32 };

// Two-level brick/voxel DDA to the nearest solid, identical to the edit/render
// march so the crater lands exactly where the cursor points.
fn marchWorld(ro : vec3<f32>, rd : vec3<f32>) -> March {
  var m : March;
  m.found = false;
  let inv = vec3<f32>(1.0) / rd;
  let t0 = (vec3<f32>(0.0) - ro) * inv;
  let t1 = (vec3<f32>(f32(WG)) - ro) * inv;
  let tenter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z));
  let texit = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
  if (texit < max(tenter, 0.0)) { return m; }
  let tstart = max(tenter, 0.0) + 0.0001;
  let stepi = vec3<i32>(sign(rd));
  let stepf = vec3<f32>(sign(rd));
  let pos0 = max(stepf, vec3<f32>(0.0));
  var brick = clamp(vec3<i32>(floor((ro + rd * tstart) / 8.0)),
                    vec3<i32>(0), vec3<i32>(BG - 1));
  var tMaxB = ((vec3<f32>(brick) + pos0) * 8.0 - ro) * inv;
  let tDeltaB = abs(inv) * 8.0;
  var tEnter = tstart;
  for (var ob = 0; ob < 210; ob = ob + 1) {
    if (brick.x < 0 || brick.y < 0 || brick.z < 0 || brick.x >= BG || brick.y >= BG || brick.z >= BG) { break; }
    let bidx = u32(brick.x + brick.y * BG + brick.z * BG * BG);
    if (bricks[bidx].occupancy != 0u) {
      let slot = bricks[bidx].pointer;
      let bmin = brick * 8;
      var voxel = clamp(vec3<i32>(floor(ro + rd * (tEnter + 0.0001))), bmin, bmin + vec3<i32>(7));
      var tMaxV = ((vec3<f32>(voxel) + pos0) - ro) * inv;
      var tVox = tEnter;
      for (var iv = 0; iv < 26; iv = iv + 1) {
        let l = voxel - bmin;
        if ((voxCur[slot * 512u + u32(l.x + l.y * 8 + l.z * 64)] & 3u) != 0u) {
          m.found = true; m.voxel = voxel; m.t = tVox; return m;
        }
        if (tMaxV.x < tMaxV.y && tMaxV.x < tMaxV.z) {
          tVox = tMaxV.x; voxel.x += stepi.x; tMaxV.x += abs(inv.x);
          if (voxel.x < bmin.x || voxel.x > bmin.x + 7) { break; }
        } else if (tMaxV.y < tMaxV.z) {
          tVox = tMaxV.y; voxel.y += stepi.y; tMaxV.y += abs(inv.y);
          if (voxel.y < bmin.y || voxel.y > bmin.y + 7) { break; }
        } else {
          tVox = tMaxV.z; voxel.z += stepi.z; tMaxV.z += abs(inv.z);
          if (voxel.z < bmin.z || voxel.z > bmin.z + 7) { break; }
        }
      }
    }
    if (tMaxB.x < tMaxB.y && tMaxB.x < tMaxB.z) {
      brick.x += stepi.x; tEnter = tMaxB.x; tMaxB.x += tDeltaB.x;
    } else if (tMaxB.y < tMaxB.z) {
      brick.y += stepi.y; tEnter = tMaxB.y; tMaxB.y += tDeltaB.y;
    } else {
      brick.z += stepi.z; tEnter = tMaxB.z; tMaxB.z += tDeltaB.z;
    }
  }
  return m;
}

var<workgroup> center : vec3<i32>;
var<workgroup> found : u32;

@compute @workgroup_size(64)
fn detonate(@builtin(local_invocation_index) lid : u32) {
  if (lid == 0u) {
    found = 0u;
    let m = marchWorld(bi.v0.xyz, normalize(bi.v1.xyz));
    if (m.found) { found = 1u; center = m.voxel; }
    let hitFlag = select(0u, 1u, m.found);
    if (m.found) {
      blast[0] = f32(m.voxel.x);
      blast[1] = f32(m.voxel.y);
      blast[2] = f32(m.voxel.z);
    }
    blast[3] = bi.v0.w; // radius
    blast[4] = bi.v1.w; // force
    blast[5] = f32(hitFlag);
    // Seed the world-fell window on the crater. carveHit[0]=0 (world, not a body)
    // so winMark/registerRoots/extract proceed; carveHit[5] left 0 (the carve-hold
    // gate is separate -- the fell is forced this frame by the CPU on a blast).
    carveHit[0] = 0u;
    carveHit[1] = 0xFFFFFFFFu;
    if (m.found) {
      carveHit[2] = u32(m.voxel.x);
      carveHit[3] = u32(m.voxel.y);
      carveHit[4] = u32(m.voxel.z);
    }
  }
  workgroupBarrier();
  if (found == 0u) { return; }

  // Carve the crater sphere: every solid voxel in radius is EJECTED as outward
  // ballistic debris (carrying its material) and cleared from both buffers + marked
  // dirty. The fell then turns the now-disconnected surroundings into rigid bodies,
  // and the impulse pass flings them outward.
  let R = i32(bi.v0.w);
  let force = bi.v1.w;
  let cf = vec3<f32>(f32(center.x) + 0.5, f32(center.y) + 0.5, f32(center.z) + 0.5);
  let dim = 2 * R + 1;
  let total = dim * dim * dim;
  for (var idx = i32(lid); idx < total; idx = idx + 64) {
    let dx = idx % dim - R;
    let dy = (idx / dim) % dim - R;
    let dz = idx / (dim * dim) - R;
    if (dx * dx + dy * dy + dz * dz > R * R) { continue; }
    let wx = center.x + dx;
    let wy = center.y + dy;
    let wz = center.z + dz;
    if (!inW(wx, wy, wz)) { continue; }
    let i = vIdx(wx, wy, wz);
    let v = voxCur[i];
    let r2 = dx * dx + dy * dy + dz * dz;
    let hh = hash3(u32(wx), u32(wy), u32(wz), 0u);
    if (r2 * 3 < R * R) {
      // Core: vaporise to a rising smoke puff (the gas CA carries + dissipates it),
      // not ejected. Sparse so it reads as a plume, not a solid ball. Smoke is
      // dynamic -> src buffer only; clear the other.
      if ((hh % 5u) < 2u) {
        let lifeS = 45u + (hh % 180u); // wide spread so the plume thins unevenly
        voxCur[i] = vox(MAT_SMOKE, CAT_GAS) | ((hh & 3u) << 2u) | (lifeS << 16u);
      } else {
        voxCur[i] = 0u;
      }
      voxOther[i] = 0u;
    } else {
      // Shell: eject solid material as outward ballistic debris (never matId 0,
      // which would render/settle black).
      if ((v & 3u) == 1u && matId(v) != 0u) {
        let p = vec3<f32>(f32(wx) + 0.5, f32(wy) + 0.5, f32(wz) + 0.5);
        var dir = p - cf;
        let dlen = length(dir);
        dir = select(dir / dlen, vec3<f32>(0.0, 1.0, 0.0), dlen < 0.001);
        let jit = (vec3<f32>(f32(hh & 0xFFu),
                             f32((hh >> 8u) & 0xFFu),
                             f32((hh >> 16u) & 0xFFu)) / 255.0 - 0.5) * 0.9;
        let vel = (dir + jit) * force * 0.95 + vec3<f32>(0.0, force * 0.2, 0.0);
        let slot = atomicAdd(&debrisHead[0], 1u) % DEBRIS_MAX;
        debris[slot].a = vec4<f32>(p, 3.0); // 3 s life
        debris[slot].b = vec4<f32>(vel, bitcast<f32>(v));
      }
      voxCur[i] = 0u;
      voxOther[i] = 0u;
    }
    atomicOr(&dirty[u32((wx / 8) + (wy / 8) * BG + (wz / 8) * BG * BG)], 1u);
  }
}
