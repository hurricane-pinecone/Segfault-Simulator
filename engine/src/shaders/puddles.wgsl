// Where rain water stands on a terrain (see puddles.h and PuddleModule): its
// puddle map, worked out over a window of the world's XY each time its
// ground, its deformation or its settings change. Six passes, each a texel
// of the window a thread:
//
// 1. cs_height: the ground's height (the mesh's, less the micro relief, plus
//    how far it's pressed in) into the work texture's x and y.
// 2. cs_spreadX, cs_spreadY: its highest (x) and lowest (y) within the
//    radius, a square, one axis at a time.
// 3. cs_closeX, cs_closeY: the highest's lowest within the radius again (x):
//    the closing, each hollow narrower than the radius filled to its brim.
//    The lowest (y), each hollow's bottom, is kept.
// 4. cs_puddles: how far up its hollow's water line each texel is, how
//    deep the hollow's water is at the line, and how fast it fills there
//    (the puddle map; see terrainPuddles in meshTerrainSurface.wgsl).
// Must match PuddleUniforms in webGpuRenderer.cpp.

struct Puddles {
  window: vec4f,     // xy its corner (world), z a texel (m), w texels across
  ground: vec4f,     // the mesh under it: xy its corner, z a texel, w texels across
  shape: vec4f,      // x the most a hollow holds (m), y the radius (texels), z micro relief (m), w its size (m)
  deform: vec4f,     // x 1 pressed in (0 not), y its texel (m), z its window's texels across,
                     // w the deepest water the map holds (m)
  deformFrom: vec4i, // xy its window's first texel (world texels)
}

@group(0) @binding(0) var<uniform> puddles: Puddles;
@group(0) @binding(1) var ground: texture_2d<f32>;    // height, u, v, pooling (-1 off the mesh)
@group(0) @binding(2) var deformMap: texture_2d<f32>; // r the offset (m), wrapping
@group(0) @binding(3) var source: texture_2d<f32>;    // the last pass's
@group(0) @binding(4) var work: texture_storage_2d<rg32float, write>;
@group(0) @binding(5) var puddleMap: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(6) var fillRate: texture_2d<f32>;  // r how fast water rises, over the mesh's UVs

// Off the mesh: so low water drains toward it.
const kOff = -1.0e4;
// Shallower than this (m), a hollow holds no water: the mesh's own
// unevenness laid onto a grid, not a low spot.
const kShallowest = 0.004;

fn windowSize() -> i32 {
  return i32(puddles.window.w);
}

// A texel's centre (world XY).
fn texelCenter(t: vec2i) -> vec2f {
  return puddles.window.xy + (vec2f(t) + 0.5) * puddles.window.z;
}

// A lattice point's random value, 0 to 1 (as puddles.cpp had it).
fn latticeHash(x: i32, y: i32) -> f32 {
  var h = (bitcast<u32>(x) * 0x8da6b343u) ^ (bitcast<u32>(y) * 0xd8163841u);
  h = h ^ (h >> 13u);
  h = h * 0x5bd1e995u;
  h = h ^ (h >> 15u);
  return f32(h & 0xffffffu) / f32(0xffffff);
}

// Smooth value noise, 0 to 1.
fn valueNoise(p: vec2f) -> f32 {
  let cell = floor(p);
  let f = p - cell;
  let u = f * f * (3.0 - 2.0 * f);
  let x = i32(cell.x);
  let y = i32(cell.y);
  return mix(mix(latticeHash(x, y), latticeHash(x + 1, y), u.x),
             mix(latticeHash(x, y + 1), latticeHash(x + 1, y + 1), u.x), u.y);
}

// How deep the micro relief dips at a point, 0 to 1: scattered, rounded
// hollows about `size` across.
fn microReliefDip(point: vec2f, size: f32) -> f32 {
  let p = point / max(size, 1e-3);
  let n = 0.7 * valueNoise(p) + 0.3 * valueNoise(p * 2.13 + vec2f(31.7, 7.3));
  return smoothstep(0.56, 0.8, n);
}

// The mesh under a point (bilinear): height, uv, pooling; w -1 off it (or
// near its edge).
fn groundAt(p: vec2f) -> vec4f {
  let size = i32(puddles.ground.w);
  let g = (p - puddles.ground.xy) / puddles.ground.z - 0.5;
  let cell = floor(g);
  let f = g - cell;
  let i = vec2i(cell);
  let last = vec2i(size - 1);
  let a = textureLoad(ground, clamp(i, vec2i(0), last), 0);
  let b = textureLoad(ground, clamp(i + vec2i(1, 0), vec2i(0), last), 0);
  let c = textureLoad(ground, clamp(i + vec2i(0, 1), vec2i(0), last), 0);
  let d = textureLoad(ground, clamp(i + vec2i(1, 1), vec2i(0), last), 0);
  if (min(min(a.w, b.w), min(c.w, d.w)) < 0.0) {
    return vec4f(0.0, 0.0, 0.0, -1.0);
  }
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// How far the ground's pressed in under a texel: the lowest of its
// deformation's texels there (so a rut narrower than it counts), fading
// out over the last 3 m of the deformation's window.
fn pressedAt(p: vec2f) -> f32 {
  if (puddles.deform.x < 0.5) {
    return 0.0;
  }
  let texel = puddles.deform.y;
  let size = i32(puddles.deform.z);
  let first = puddles.deformFrom.xy;
  let half = 0.5 * puddles.window.z;
  let low = vec2i(floor((p - half) / texel));
  let high = min(vec2i(floor((p + half) / texel)), low + vec2i(7));
  var lowest = 1.0e9;
  for (var y = low.y; y <= high.y; y++) {
    for (var x = low.x; x <= high.x; x++) {
      let t = vec2i(x, y);
      if (any(t < first) || any(t >= first + vec2i(size))) {
        continue;
      }
      let at = ((t % size) + size) % size;
      lowest = min(lowest, textureLoad(deformMap, at, 0).r);
    }
  }
  if (lowest > 1.0e8) {
    return 0.0;
  }
  let local = p - vec2f(first) * texel;
  let extent = f32(size) * texel;
  let edge = min(min(local.x, local.y), min(extent - local.x, extent - local.y));
  return lowest * clamp(edge / 3.0, 0.0, 1.0);
}

// The ground's height at a texel, as water stands on it (kOff off the
// mesh).
fn heightAt(t: vec2i) -> f32 {
  let p = texelCenter(t);
  let g = groundAt(p);
  if (g.w < 0.0) {
    return kOff;
  }
  let dip = puddles.shape.z * g.w * microReliefDip(p, puddles.shape.w);
  return g.x - dip + pressedAt(p);
}

fn inside(t: vec2u) -> bool {
  return all(t < vec2u(u32(windowSize())));
}

fn load(t: vec2i) -> vec2f {
  return textureLoad(source, clamp(t, vec2i(0), vec2i(windowSize() - 1)), 0).xy;
}

@compute @workgroup_size(8, 8)
fn cs_height(@builtin(global_invocation_id) id: vec3u) {
  if (!inside(id.xy)) {
    return;
  }
  let h = heightAt(vec2i(id.xy));
  textureStore(work, vec2i(id.xy), vec4f(h, h, 0.0, 0.0));
}

// The highest (x) and lowest (y) along an axis.
fn spread(id: vec2u, axis: vec2i) {
  if (!inside(id)) {
    return;
  }
  let t = vec2i(id);
  let radius = i32(puddles.shape.y);
  var highest = -1.0e9;
  var lowest = 1.0e9;
  for (var i = -radius; i <= radius; i++) {
    let v = load(t + axis * i);
    highest = max(highest, v.x);
    lowest = min(lowest, v.y);
  }
  textureStore(work, t, vec4f(highest, lowest, 0.0, 0.0));
}

@compute @workgroup_size(8, 8)
fn cs_spreadX(@builtin(global_invocation_id) id: vec3u) {
  spread(id.xy, vec2i(1, 0));
}

@compute @workgroup_size(8, 8)
fn cs_spreadY(@builtin(global_invocation_id) id: vec3u) {
  spread(id.xy, vec2i(0, 1));
}

// The lowest of x along an axis (y kept).
fn close(id: vec2u, axis: vec2i) {
  if (!inside(id)) {
    return;
  }
  let t = vec2i(id);
  let radius = i32(puddles.shape.y);
  var lowest = 1.0e9;
  for (var i = -radius; i <= radius; i++) {
    lowest = min(lowest, load(t + axis * i).x);
  }
  textureStore(work, t, vec4f(lowest, load(t).y, 0.0, 0.0));
}

@compute @workgroup_size(8, 8)
fn cs_closeX(@builtin(global_invocation_id) id: vec3u) {
  close(id.xy, vec2i(1, 0));
}

@compute @workgroup_size(8, 8)
fn cs_closeY(@builtin(global_invocation_id) id: vec3u) {
  close(id.xy, vec2i(0, 1));
}

@compute @workgroup_size(8, 8)
fn cs_puddles(@builtin(global_invocation_id) id: vec3u) {
  if (!inside(id.xy)) {
    return;
  }
  let t = vec2i(id.xy);
  let h = heightAt(t);
  let brim = load(t).x;
  let bottom = min(load(t).y, h);
  // Where the brim slopes, water runs off rather than standing: a rut down a
  // hill stays wet, not full.
  let across = 2.0 * puddles.window.z;
  let slope = length(vec2f(load(t + vec2i(1, 0)).x - load(t - vec2i(1, 0)).x,
                           load(t + vec2i(0, 1)).x - load(t - vec2i(0, 1)).x)) / across;
  let runoff = 1.0 - smoothstep(0.03, 0.08, slope);
  let full = (min(brim, bottom + puddles.shape.x) - bottom) * runoff;
  // How fast water rises here (blue, for the terrain), at the mesh's UV: on
  // every texel, dry ones too, so it doesn't blend down at a puddle's edge.
  let g = groundAt(texelCenter(t));
  var rate = 1.0;
  if (g.w >= 0.0) {
    let size = vec2i(textureDimensions(fillRate));
    rate = textureLoad(fillRate, clamp(vec2i(g.yz * vec2f(size)), vec2i(0), size - 1), 0).r;
  }
  var texel = vec4f(1.0, 0.0, rate, 1.0); // dry
  if (h > kOff * 0.5 && bottom > kOff * 0.5 && full >= kShallowest && h < bottom + full) {
    let level = clamp((h - bottom) / full, 0.0, 1.0);
    texel = vec4f(level * 254.0 / 255.0, full / puddles.deform.w, rate, 1.0);
  }
  // The map's first row is its +y edge (as the terrain samples it).
  textureStore(puddleMap, vec2i(t.x, windowSize() - 1 - t.y), texel);
}
