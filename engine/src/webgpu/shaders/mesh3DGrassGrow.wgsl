// Grows a grass field's blades for a frame (a compute pass before the lit
// pass), from its ground and maps: only those in view, nearest first in
// detail, into three lists (a blade's detail: 5, 2 or 1 segments) that the lit
// pass draws indirectly (mesh3DGrass.wgsl).
//
// Blades are spread over the ground's own XY on four lattices, each twice as
// coarse as the one before and reaching twice-ish as far. Each lattice cell
// has one blade, at a random place in it (from a hash of the cell, so it's
// there every frame). A cell whose coordinates are both even is left to the
// next lattice's, so they add up to one blade per finest cell near the
// camera, thinning to a quarter, a sixteenth... farther out, each blade
// shrinking away as its lattice's reach ends rather than popping.

struct Grow {
  model: mat4x4f,         // the ground's own space to the world
  normalMatrix: mat4x4f,  // its normals to the world's (upper 3 x 3)
  planes: array<vec4f, 6>, // the view frustum's, inward (dot(xyz, p) + w >= 0)
  camera: vec4f,          // xyz the camera, w draw distance
  ground: vec4f,          // xy the ground grid's min, zw its texel size
  groundSize: vec4i,      // xy texels
  maps: vec4f,            // xy the maps' grid min, zw 1 / its size (0: unpainted)
  shape: vec4f,           // height, height variation, width, finest cell
  reach: vec4f,           // each lattice's reach (world units)
  detail: vec4f,          // x 5 segments to here, y 2 to here (world units)
  capacity: vec4u,        // each list's room (blades), and w: 0
  lattices: array<vec4i, 4>, // each one's first cell (xy) and how many (zw)
}

// A grown blade, 32 bytes:
//   root: xyz its world root, w its height
//   look.x: its ground's world normal, xy (snorm16 x 2; z up, from them)
//   look.y: width and segments (float16 x 2)
//   look.z: facing (of a turn), lean, random, tone (unorm8 x 4)
//   look.w: where it is on the maps (unorm16 x 2)
struct GrownBlade {
  root: vec4f,
  look: vec4u,
}

@group(0) @binding(0) var<uniform> grow: Grow;
@group(0) @binding(1) var groundMap: texture_2d<f32>; // xyz normal, w height
@group(0) @binding(2) var shapeMap: texture_2d<f32>;  // r coverage, g length
@group(0) @binding(3) var mapSampler: sampler;
@group(0) @binding(4) var<storage, read_write> blades: array<GrownBlade>;
// Three indirect draws (vertex count, instance count, first vertex, first
// instance), each list's.
@group(0) @binding(5) var<storage, read_write> draws: array<atomic<u32>, 12>;

fn pcg(value: u32) -> u32 {
  let state = value * 747796405u + 2891336453u;
  let word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}

// The next of a cell's random numbers, 0 (inclusive) to 1 (exclusive).
fn random(seed: ptr<function, u32>) -> f32 {
  *seed = pcg(*seed);
  return f32(*seed >> 8u) / 16777216.0;
}

fn hasGround(texel: vec4f) -> bool {
  return any(texel.xyz != vec3f(0.0));
}

// The ground's height at a point (texels), between the texels around it that
// have ground.
fn heightAt(at: vec2f, fallback: f32) -> f32 {
  let p = at - 0.5;
  let low = vec2i(floor(p));
  let t = p - floor(p);
  var total = 0.0;
  var weight = 0.0;
  for (var k = 0; k < 4; k++) {
    let offset = vec2i(k & 1, k >> 1);
    let texel = low + offset;
    if (any(texel < vec2i(0)) || any(texel >= grow.groundSize.xy)) {
      continue;
    }
    let g = textureLoad(groundMap, texel, 0);
    if (!hasGround(g)) {
      continue;
    }
    let w = select(1.0 - t.x, t.x, offset.x == 1) * select(1.0 - t.y, t.y, offset.y == 1);
    total += g.w * w;
    weight += w;
  }
  return select(fallback, total / weight, weight > 1e-5);
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) id: vec3u) {
  let lattice = id.z;
  let span = grow.lattices[lattice];
  if (i32(id.x) >= span.z || i32(id.y) >= span.w) {
    return;
  }
  let cell = span.xy + vec2i(id.xy);
  if (lattice < 3u && (cell.x & 1) == 0 && (cell.y & 1) == 0) {
    return; // the next lattice's
  }
  var seed = pcg(bitcast<u32>(cell.x) ^ pcg(bitcast<u32>(cell.y) ^ pcg(lattice + 40503u)));
  let size = grow.shape.w * f32(1u << lattice);
  let local = grow.ground.xy + (vec2f(cell) + vec2f(random(&seed), random(&seed))) * size;

  // The ground under it.
  let at = (local - grow.ground.xy) / grow.ground.zw;
  let texel = vec2i(floor(at));
  if (any(texel < vec2i(0)) || any(texel >= grow.groundSize.xy)) {
    return;
  }
  let ground = textureLoad(groundMap, texel, 0);
  if (!hasGround(ground)) {
    return;
  }
  let root = (grow.model * vec4f(local, heightAt(at, ground.w), 1.0)).xyz;
  let away = distance(root, grow.camera.xyz);
  let reach = grow.reach[lattice];
  if (away > reach) {
    return;
  }

  // As its maps say: whether it grows here, and how long.
  let uv = (local - grow.maps.xy) * grow.maps.zw;
  let maps = textureSampleLevel(shapeMap, mapSampler, uv, 0.0);
  let keep = random(&seed);
  let tall = random(&seed);
  if (keep >= maps.r) {
    return;
  }
  var height = grow.shape.x * max(1.0 + grow.shape.y * (tall * 2.0 - 1.0) * 0.5, 0.05) * maps.g;
  height *= 1.0 - smoothstep(reach * 0.7, reach, away); // shrinking away
  if (height < 0.004) {
    return;
  }

  // In view: a sphere round it, with room to lean.
  let middle = root + vec3f(0.0, 0.0, height * 0.5);
  for (var p = 0; p < 6; p++) {
    if (dot(grow.planes[p].xyz, middle) + grow.planes[p].w < -height * 1.2) {
      return;
    }
  }

  // Its detail: fewer segments farther away, and for short grass (mown
  // grass hardly curves). Short grass near the camera goes in a coarser list
  // while it has room, else in its distance's.
  let far = select(select(2u, 1u, away < grow.detail.y), 0u, away < grow.detail.x);
  var list = max(far, select(select(0u, 1u, height < 0.22), 2u, height < 0.1));
  var index = atomicAdd(&draws[list * 4u + 1u], 1u);
  if (index >= grow.capacity[list] && list != far) {
    atomicSub(&draws[list * 4u + 1u], 1u);
    list = far;
    index = atomicAdd(&draws[list * 4u + 1u], 1u);
  }
  if (index >= grow.capacity[list]) {
    atomicSub(&draws[list * 4u + 1u], 1u); // out of room: one fewer
    return;
  }
  var first = 0u;
  for (var l = 0u; l < list; l++) {
    first += grow.capacity[l];
  }

  // Coarser lattices' blades are wider, so a thinner field still reads full.
  let width = grow.shape.z * (1.0 + 0.45 * f32(lattice));
  let normal = normalize((grow.normalMatrix * vec4f(ground.xyz, 0.0)).xyz);
  let facing = random(&seed);
  let lean = random(&seed);
  let blade = random(&seed);
  let tone = random(&seed);
  blades[first + index] = GrownBlade(
      vec4f(root, height),
      vec4u(pack2x16snorm(normal.xy),
            pack2x16float(vec2f(width, f32(select(select(1u, 2u, list == 1u), 5u, list == 0u)))),
            pack4x8unorm(vec4f(facing, lean, blade, tone)),
            pack2x16unorm(clamp(uv, vec2f(0.0), vec2f(1.0)))));
}
