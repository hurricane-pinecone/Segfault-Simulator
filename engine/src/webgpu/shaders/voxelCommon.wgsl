// Shared prelude prepended to every voxel compute/render stage.
// Voxel = (r<<24)|(g<<16)|(b<<8)|low. low: bits0-1 type (0 air/1 solid/2 water),
// bits2-3 = water's remembered flow direction (0 +x, 1 -x, 2 +z, 3 -z).
const WG : i32 = 512;
const BG : i32 = 64;
const SEA : i32 = 96;
const MAXB : u32 = 32u; // rigid-body pool size (must match kMaxBodies)
const BODYDIM : i32 = 96;       // rigid-body / fell-window grid edge (== kBodyDim)
const BODYVOX : u32 = 884736u;  // BODYDIM^3, one slot's grid
struct Brick { occupancy : u32, pointer : u32 };
fn hash3(x : u32, y : u32, z : u32, f : u32) -> u32 {
  var h = x * 374761393u + y * 668265263u + z * 2246822519u + f * 3266489917u;
  h = (h ^ (h >> 13u)) * 1274126177u;
  return h ^ (h >> 16u);
}
