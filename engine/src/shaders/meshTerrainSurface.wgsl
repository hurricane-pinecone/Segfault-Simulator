// A terrain's surface (MeshMaterial::terrain, see TerrainMaterial): up to five
// ground layers blended by a splat map over the mesh's UVs. Each layer's
// textures lie over the world's XY from above, `tileSize` across, broken up
// so a big field doesn't repeat: sampled at three randomly shifted and turned
// copies on a hexagonal grid, blended (Mikkelsen's hex-tiling, after Heitz
// and Neyret's triangle grid), with a gentle brightness variation over tens
// of metres. Layers blend by their heights (the colour's alpha); a tint over
// the mesh's UVs multiplies the result (white without one). Layers marked as
// meadow look like grass seen from afar (see meadowLook). Triplanar layers
// are also laid from the sides on steep ground (see terrainProjections).
// Must match TerrainUniforms in framePlan.h.

struct Terrain {
  layers: array<vec4f, 5>,  // rgb tint, w 1 / its tile size
  detail: array<vec4f, 5>,  // x roughness, y normal scale, z meadow, w triplanar exponent (0: off)
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
// tilt along its plane's two axes, or xyz along the world's once it's back in
// the world (terrainProjected); b, or then w, roughness).
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

// A layer at a point on its plane (the world's xy from above), tiled (and,
// with anti-tiling, hex-tiled).
fn terrainLayer(layer: i32, world: vec2f, dx: vec2f, dy: vec2f) -> LayerSample {
  let scale = terrain.layers[layer].w;
  // V runs down the image: the plane's +y up it, so the normal map's green
  // is +y.
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

// The plane a projection lies in: 0 from the side along x (the world's yz),
// 1 along y (xz), 2 from above (xy). Upright on the walls: up the image is up.
fn terrainPlane(v: vec3f, axis: i32) -> vec2f {
  if (axis == 0) {
    return v.yz;
  }
  if (axis == 1) {
    return v.xz;
  }
  return v.xy;
}

// A layer projected along `axis` (see terrainPlane), its normal's tilt back
// along the world's axes: detail xyz the tilt, w roughness.
fn terrainProjected(layer: i32, world: vec3f, dx: vec3f, dy: vec3f, axis: i32) -> LayerSample {
  var out = terrainLayer(layer, terrainPlane(world, axis), terrainPlane(dx, axis), terrainPlane(dy, axis));
  let tilt = out.detail.xy;
  var along = vec3f(tilt, 0.0);
  if (axis == 0) {
    along = vec3f(0.0, tilt.x, tilt.y);
  } else if (axis == 1) {
    along = vec3f(tilt.x, 0.0, tilt.y);
  }
  out.detail = vec4f(along, out.detail.z);
  return out;
}

// Biplanar mapping (after Quilez): the two axes the surface faces most, the
// first and second, and how much each shows (before a layer's exponent).
// Their weights are remapped so the second fades out before it could swap
// with the third (no seam), and below about 35 degrees of slope it's gone:
// gentle ground stays a single projection from above.
struct Projections {
  first: i32,
  second: i32,
  weights: vec2f,
}

fn terrainProjections(normal: vec3f) -> Projections {
  let n = abs(normal);
  var out: Projections;
  out.first = select(select(2, 1, n.y > n.z), 0, n.x > n.y && n.x > n.z);
  let a = (out.first + 1) % 3;
  let b = (out.first + 2) % 3;
  out.second = select(b, a, n[a] >= n[b]);
  let weights = vec2f(n[out.first], n[out.second]);
  out.weights = clamp((weights - 0.57735027) / (1.0 - 0.57735027), vec2f(0.0), vec2f(1.0));
  return out;
}

// A layer at a world point: from above, or for a triplanar layer, from the
// one or two sides its ground faces (where the second shows at all).
fn terrainLayerAt(layer: i32, world: vec3f, dx: vec3f, dy: vec3f, projections: Projections) -> LayerSample {
  let exponent = terrain.detail[layer].w;
  if (exponent <= 0.0) {
    return terrainProjected(layer, world, dx, dy, 2);
  }
  let w = pow(projections.weights, vec2f(exponent));
  let second = w.y / max(w.x + w.y, 1e-6);
  let first = terrainProjected(layer, world, dx, dy, projections.first);
  if (second < 0.01) {
    return first;
  }
  let other = terrainProjected(layer, world, dx, dy, projections.second);
  var out: LayerSample;
  out.color = mix(first.color, other.color, second);
  out.detail = mix(first.detail, other.detail, second);
  return out;
}

// Grass seen from afar, past where its blades are drawn (a layer's meadow):
// its colour broken up in patches, a little yellower or bluer, lighter or
// darker; light and dark bands rolling across it with the wind, as its gusts
// roll over the blades (grassBlade.wgsl), showing more or less of their paler
// sides; and a soft sheen at low angles, where the eye sees the blades' lit
// sides rather than the shadowed gaps between them. `pixel`: how many metres
// a pixel spans there, so detail finer than that fades rather than shimmers.
fn meadowLook(color: vec3f, world: vec2f, normal: vec3f, toView: vec3f, pixel: f32) -> vec3f {
  let coarse = terrainNoise(world * 0.09 + 41.0) - 0.5;
  let patches = (terrainNoise(world * 0.33 + 7.0) - 0.5) * (1.0 - smoothstep(0.6, 1.5, pixel * 0.33));
  let fine = (terrainNoise(world * 1.7 + 13.0) - 0.5) * (1.0 - smoothstep(0.3, 0.8, pixel * 1.7));
  var look = color * (1.0 + patches * 0.3 + fine * 0.22);
  look *= vec3f(1.0 + coarse * 0.22, 1.0, 1.0 - coarse * 0.3);

  let wind = frame.wind;
  let wave = sin(dot(world, wind.xy) * 0.35 - frame.time.x * 1.7) * 0.5 + 0.5;
  let gusts = mix(0.75, 0.3 + wave * 1.1, wind.w);
  look *= 1.0 + (gusts - 0.75) * 0.2 * clamp(wind.z, 0.0, 1.5);

  let grazing = 1.0 - clamp(dot(toView, normal), 0.0, 1.0);
  let sheen = grazing * grazing * grazing * 0.3;
  let grey = dot(look, vec3f(0.3, 0.59, 0.11));
  return mix(look, look * 1.15 + vec3f(grey * 0.12), sheen);
}

fn surface(in: SurfaceIn) -> Surface {
  // In uniform control flow: the splat map, and the derivatives every
  // layer's sampling uses (so the layers can be skipped where they're not).
  let splat = textureSample(splatMap, splatSampler, in.uv);
  let tint = textureSample(tintMap, splatSampler, in.uv).rgb;
  let world = in.position.xy;
  let dx3 = dpdx(in.position);
  let dy3 = dpdy(in.position);
  let dx = dx3.xy;
  let dy = dy3.xy;
  let projections = terrainProjections(in.normal);

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
      samples[i] = terrainLayerAt(layer, in.position, dx3, dy3, projections);
      tallest = max(tallest, samples[i].color.a + weights[i]);
    }
  }
  let band = mix(1.0, 0.04, clamp(terrain.params.y, 0.0, 1.0));
  var color = vec3f(0.0);
  var tilt = vec3f(0.0);
  var roughness = 0.0;
  var meadow = 0.0;
  var total = 0.0;
  for (var i = 0; i < 5; i++) {
    if (weights[i] <= 0.002) {
      continue;
    }
    let layer = select(0, i, i < count);
    let blend = max(samples[i].color.a + weights[i] - tallest + band, 0.0) * weights[i];
    color += samples[i].color.rgb * terrain.layers[layer].rgb * blend;
    tilt += samples[i].detail.xyz * terrain.detail[layer].y * blend;
    roughness += samples[i].detail.w * terrain.detail[layer].x * blend;
    meadow += terrain.detail[layer].z * blend;
    total += blend;
  }
  let share = 1.0 / max(total, 1e-5);
  color *= share;
  tilt *= share;
  roughness *= share;
  meadow *= share;

  // Brightness drifting gently over tens of metres, so a field isn't uniform.
  let drift = terrainNoise(world * 0.04) * 0.65 + terrainNoise(world * 0.13) * 0.35;
  color *= (1.0 + (drift - 0.5) * 2.0 * terrain.params.w) * tint;
  if (meadow > 0.002) {
    let pixel = max(length(dx), length(dy));
    color = mix(color, meadowLook(color, world, in.normal, in.toView, pixel), meadow);
  }

  // The tilt is along the world's axes: laid into the surface's own plane
  // (each projection's tilt as a world-space nudge, then the part along the
  // normal dropped).
  let up = in.normal;
  let bent = normalize(up + tilt - up * dot(up, tilt));

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
