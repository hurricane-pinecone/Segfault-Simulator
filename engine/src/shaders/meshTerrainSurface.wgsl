// A terrain's surface (MeshMaterial::terrain, see TerrainMaterial): up to five
// ground layers blended by a splat map over the mesh's UVs. Each layer's
// textures lie over the world's XY from above, `tileSize` across, broken up
// so a big field doesn't repeat: sampled at three randomly shifted and turned
// copies on a hexagonal grid, blended (Mikkelsen's hex-tiling, after Heitz
// and Neyret's triangle grid), with a gentle brightness variation over tens
// of metres. Layers blend by their heights (the colour's alpha); a tint over
// the mesh's UVs multiplies the result (white without one). Must match
// TerrainUniforms in framePlan.h.

struct Terrain {
  layers: array<vec4f, 5>,  // rgb tint, w 1 / its tile size
  detail: array<vec4f, 5>,  // x roughness, y normal scale
  params: vec4f,            // x layers, y blend sharpness, z anti-tiling (0/1), w variation
}

@group(1) @binding(0) var<uniform> terrain: Terrain;
@group(1) @binding(1) var splatMap: texture_2d<f32>;    // rgba layers 2-5's weights (data)
@group(1) @binding(2) var splatSampler: sampler;        // smooth, clamped at the edges
@group(1) @binding(3) var colorLayers: texture_2d_array<f32>;  // sRGB: rgb colour, a height
@group(1) @binding(4) var detailLayers: texture_2d_array<f32>; // data: rg normal xy, b roughness
@group(1) @binding(5) var layerSampler: sampler;        // repeating, mipmapped
@group(1) @binding(6) var tintMap: texture_2d<f32>;     // sRGB: x the colour, over the mesh's UVs

// A point's random 2D offset (0..1) for a hex-grid vertex.
fn terrainHash(p: vec2f) -> vec2f {
  let q = vec2f(dot(p, vec2f(127.1, 311.7)), dot(p, vec2f(269.5, 183.3)));
  return fract(sin(q) * 43758.5453);
}

// Smooth value noise, 0..1.
fn terrainNoise(p: vec2f) -> f32 {
  let cell = floor(p);
  let f = p - cell;
  let u = f * f * (3.0 - 2.0 * f);
  let a = terrainHash(cell).x;
  let b = terrainHash(cell + vec2f(1.0, 0.0)).x;
  let c = terrainHash(cell + vec2f(0.0, 1.0)).x;
  let d = terrainHash(cell + vec2f(1.0, 1.0)).x;
  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// One layer sampled: colour (rgb, a height), and its detail (rg the normal's
// tilt, along the world's x and y; b roughness).
struct LayerSample {
  color: vec4f,
  detail: vec4f,
}

// A layer's textures at world-tiling coordinates `st` (with their screen
// derivatives), once: rotated by `turn` (cos, sin) and shifted.
fn terrainTap(layer: i32, st: vec2f, dx: vec2f, dy: vec2f, turn: vec2f, shift: vec2f) -> LayerSample {
  let rotate = mat2x2f(turn.x, turn.y, -turn.y, turn.x);
  let at = rotate * st + shift;
  var out: LayerSample;
  out.color = textureSampleGrad(colorLayers, layerSampler, at, layer, rotate * dx, rotate * dy);
  let detail = textureSampleGrad(detailLayers, layerSampler, at, layer, rotate * dx, rotate * dy);
  // The normal's tilt back into the world's axes (its texture's were turned).
  let tilt = transpose(rotate) * (detail.rg * 2.0 - 1.0);
  out.detail = vec4f(tilt, detail.b, 1.0);
  return out;
}

// A layer at a world point, tiled (and, with anti-tiling, hex-tiled).
fn terrainLayer(layer: i32, world: vec2f, dx: vec2f, dy: vec2f) -> LayerSample {
  let scale = terrain.layers[layer].w;
  // V runs down the image: world +y up it, so the normal map's green is +y.
  let st = vec2f(world.x, -world.y) * scale;
  let sdx = vec2f(dx.x, -dx.y) * scale;
  let sdy = vec2f(dy.x, -dy.y) * scale;
  if (terrain.params.z < 0.5) {
    return terrainTap(layer, st, sdx, sdy, vec2f(1.0, 0.0), vec2f(0.0));
  }
  // The triangle of the hex grid st is in (each vertex a copy of the
  // texture), and how near each vertex it is.
  let skewed = mat2x2f(1.0, 0.0, -0.57735027, 1.15470054) * (st * 3.4641016 * 0.5);
  let base = floor(skewed);
  let f = fract(skewed);
  let z = 1.0 - f.x - f.y;
  var w = vec3f(0.0);
  var v1 = vec2f(0.0);
  var v2 = vec2f(0.0);
  var v3 = vec2f(0.0);
  if (z > 0.0) {
    w = vec3f(z, f.y, f.x);
    v1 = base;
    v2 = base + vec2f(0.0, 1.0);
    v3 = base + vec2f(1.0, 0.0);
  } else {
    w = vec3f(-z, 1.0 - f.y, 1.0 - f.x);
    v1 = base + vec2f(1.0, 1.0);
    v2 = base + vec2f(1.0, 0.0);
    v3 = base + vec2f(0.0, 1.0);
  }
  let h1 = terrainHash(v1);
  let h2 = terrainHash(v2);
  let h3 = terrainHash(v3);
  let a = terrainTap(layer, st, sdx, sdy, vec2f(cos(h1.x * 6.2831853), sin(h1.x * 6.2831853)), h1 * 7.0);
  let b = terrainTap(layer, st, sdx, sdy, vec2f(cos(h2.x * 6.2831853), sin(h2.x * 6.2831853)), h2 * 7.0);
  let c = terrainTap(layer, st, sdx, sdy, vec2f(cos(h3.x * 6.2831853), sin(h3.x * 6.2831853)), h3 * 7.0);
  // Sharpened toward the nearest copy, by brightness (Mikkelsen's
  // contrast-keeping blend), so the mix doesn't wash out.
  let luma = vec3f(dot(a.color.rgb, vec3f(0.3, 0.59, 0.11)), dot(b.color.rgb, vec3f(0.3, 0.59, 0.11)),
                   dot(c.color.rgb, vec3f(0.3, 0.59, 0.11)));
  var weight = w * w * w * (vec3f(1.0) + luma);
  weight = pow(weight, vec3f(2.0));
  weight /= max(weight.x + weight.y + weight.z, 1e-6);
  var out: LayerSample;
  out.color = a.color * weight.x + b.color * weight.y + c.color * weight.z;
  out.detail = a.detail * weight.x + b.detail * weight.y + c.detail * weight.z;
  return out;
}

fn surface(in: SurfaceIn) -> Surface {
  // In uniform control flow: the splat map, and the derivatives every
  // layer's sampling uses (so the layers can be skipped where they're not).
  let splat = textureSample(splatMap, splatSampler, in.uv);
  let tint = textureSample(tintMap, splatSampler, in.uv).rgb;
  let world = in.position.xy;
  let dx = dpdx(world);
  let dy = dpdy(world);

  // Each layer's weight: the splat map's four over the first, sharing past 1.
  let painted = clamp(splat, vec4f(0.0), vec4f(1.0));
  let sum = painted.x + painted.y + painted.z + painted.w;
  let normalized = select(painted, painted / sum, sum > 1.0);
  var weights = array<f32, 5>(max(1.0 - min(sum, 1.0), 0.0), normalized.x, normalized.y, normalized.z, normalized.w);
  let count = i32(terrain.params.x);

  // The layers there, and by height: the higher layer shows through (toward
  // a crisp edge at full sharpness).
  var samples: array<LayerSample, 5>;
  var tallest = -1.0;
  for (var i = 0; i < 5; i++) {
    let layer = select(0, i, i < count); // a channel with no layer: the first's
    if (weights[i] > 0.002) {
      samples[i] = terrainLayer(layer, world, dx, dy);
      tallest = max(tallest, samples[i].color.a + weights[i]);
    }
  }
  let band = mix(1.0, 0.04, clamp(terrain.params.y, 0.0, 1.0));
  var color = vec3f(0.0);
  var tilt = vec2f(0.0);
  var roughness = 0.0;
  var total = 0.0;
  for (var i = 0; i < 5; i++) {
    if (weights[i] <= 0.002) {
      continue;
    }
    let layer = select(0, i, i < count);
    let blend = max(samples[i].color.a + weights[i] - tallest + band, 0.0) * weights[i];
    color += samples[i].color.rgb * terrain.layers[layer].rgb * blend;
    tilt += samples[i].detail.rg * terrain.detail[layer].y * blend;
    roughness += samples[i].detail.b * terrain.detail[layer].x * blend;
    total += blend;
  }
  let share = 1.0 / max(total, 1e-5);
  color *= share;
  tilt *= share;
  roughness *= share;

  // Brightness drifting gently over tens of metres, so a field isn't uniform.
  let drift = terrainNoise(world * 0.04) * 0.65 + terrainNoise(world * 0.13) * 0.35;
  color *= (1.0 + (drift - 0.5) * 2.0 * terrain.params.w) * tint;

  // The tilt is along the world's x and y: into the surface's own tangent
  // frame.
  let up = in.normal;
  let alongX = normalize(vec3f(1.0, 0.0, 0.0) - up * up.x + vec3f(1e-5, 0.0, 0.0));
  let alongY = normalize(cross(up, alongX));
  let bent = normalize(up + alongX * tilt.x + alongY * tilt.y);

  var result: Surface;
  result.baseColor = color;
  result.alpha = 1.0;
  result.metallic = 0.0;
  result.roughness = clamp(roughness, 0.04, 1.0);
  result.normal = normalize(vec3f(dot(bent, in.tangent), dot(bent, in.bitangent), dot(bent, up)));
  result.emission = vec3f(0.0);
  result.alphaClip = 0.5;
  result.bloom = 1.0;
  return result;
}

fn surfaceAlphaMode() -> u32 {
  return ALPHA_OPAQUE;
}
