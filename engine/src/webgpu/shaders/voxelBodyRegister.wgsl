// Compacts the labeled connected components into FREE body slots (so a new cut
// drops immediately, alongside bodies still falling). A component root (a
// detached brick whose label equals its own index) grabs the next free slot from
// the CPU-supplied list; components beyond the free count and all non-roots map
// to the sentinel (their voxels stay in the world). rootSlot is indexed by root
// (= component label), so any brick later looks up its slot via
// rootSlot[label[brick]]. freeSlot[0] = free count, freeSlot[1..] = free indices.
@group(0) @binding(0) var<storage, read> labelBuf : array<u32>;
@group(0) @binding(1) var<storage, read_write> rootSlot : array<u32>;
@group(0) @binding(2) var<storage, read_write> slotCount : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read> freeSlot : array<u32>;

const SENTINEL : u32 = 0xFFFFFFFFu;

@compute @workgroup_size(64)
fn registerRoots(@builtin(global_invocation_id) gid : vec3<u32>) {
  let bi = gid.x;
  if (bi >= u32(BG) * u32(BG) * u32(BG)) { return; }
  if (labelBuf[bi] == bi) { // a detached component root
    let k = atomicAdd(&slotCount[0], 1u);
    if (k < freeSlot[0]) {
      rootSlot[bi] = freeSlot[1u + k];
    } else {
      rootSlot[bi] = SENTINEL;
    }
  } else {
    rootSlot[bi] = SENTINEL;
  }
}
