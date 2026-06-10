// Copy the detached bulk (solid voxels in not-anchored bricks) into the body
// grid (body-local = world - origin), clear them from the world, and clear any
// stale detached (bit 4) flags left behind. Dispatched over the kBodyDim^3 body
// grid; the world origin is the reduce's aabbMin (bodyMeta[0..2]).
@group(0) @binding(0) var<storage, read_write> vox0 : array<u32>;
@group(0) @binding(1) var<storage, read_write> vox1 : array<u32>;
@group(0) @binding(2) var<storage, read> anchor : array<u32>;
@group(0) @binding(3) var<storage, read> bodyMeta : array<i32>;
@group(0) @binding(4) var<storage, read_write> bodyVox : array<u32>;

const DIM : i32 = 64;

fn vIndex(x : i32, y : i32, z : i32) -> u32 {
  let bi = (x / 8) + (y / 8) * BG + (z / 8) * BG * BG;
  return u32(bi) * 512u + u32((x % 8) + (y % 8) * 8 + (z % 8) * 64);
}

@compute @workgroup_size(4, 4, 4)
fn extract(@builtin(global_invocation_id) gid : vec3<u32>) {
  let lx = i32(gid.x);
  let ly = i32(gid.y);
  let lz = i32(gid.z);
  if (lx >= DIM || ly >= DIM || lz >= DIM) { return; }
  let bodyIdx = u32(lx + ly * DIM + lz * DIM * DIM);

  let wx = bodyMeta[0] + lx;
  let wy = bodyMeta[1] + ly;
  let wz = bodyMeta[2] + lz;
  if (wx < 0 || wy < 0 || wz < 0 || wx >= WG || wy >= WG || wz >= WG) {
    bodyVox[bodyIdx] = 0u;
    return;
  }
  let vi = vIndex(wx, wy, wz);
  let v = vox0[vi];
  let brick = u32((wx / 8) + (wy / 8) * BG + (wz / 8) * BG * BG);
  if ((v & 3u) == 1u && anchor[brick] == 0u) {
    bodyVox[bodyIdx] = v & 0xFFFFFFEFu; // into the body, detached flag cleared
    vox0[vi] = 0u;
    vox1[vi] = 0u;
  } else {
    bodyVox[bodyIdx] = 0u;
    if ((v & 0x10u) != 0u) {
      vox0[vi] = v & 0xFFFFFFEFu;
      vox1[vi] = vox1[vi] & 0xFFFFFFEFu;
    }
  }
}
