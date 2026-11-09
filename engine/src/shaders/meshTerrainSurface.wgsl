// A terrain's surface (MeshMaterial::terrain, see TerrainMaterial): up to five
// ground layers blended by a splat map over the mesh's UVs. Each layer's
// textures lie over the world's XY from above, `tileSize` across, broken up
// so a big field doesn't repeat: sampled at three randomly shifted and turned
// copies on a hexagonal grid, blended (Mikkelsen's hex-tiling, after Heitz
// and Neyret's triangle grid), with a gentle brightness variation over tens
// of metres. Layers blend by their heights (the colour's alpha); a tint over
// the mesh's UVs multiplies the result (white without one), and a game may
// paint layers over the splat map's (see paintedUv). Layers marked as
// meadow look like grass seen from afar (see meadowLook). Triplanar layers
// are also laid from the sides on steep ground (see terrainProjections).
// Water stands in its low spots as the weather's puddles rise (see
// terrainPuddles). A deformable one is pressed in where things have gone over
// it (see terrainDeformed). Must match TerrainUniforms in framePlan.h.

struct Terrain {
  layers: array<vec4f, 5>,  // rgb tint, w 1 / its tile size
  detail: array<vec4f, 5>,  // x roughness, y normal scale, z meadow, w triplanar exponent (0: off)
  params: vec4f,            // x layers, y blend sharpness, z anti-tiling (0/1), w variation
  puddleMap: vec4f,         // xy the world XY of its corner, zw 1 / the size it covers
  puddleParams: vec4f,      // x has puddles (0/1), y the deepest water its map holds (m)
  puddleLayers: array<vec4f, 2>, // each layer's puddles: layers 1-4, then 5
  deformMap: vec4f,         // x 0 not deformed, 1 the ground, 2 its patch; y a texel (m), z 1 / the
                            // window's size (m), w the deepest it's pressed (m, <= 0)
  deformWindow: vec4f,      // the window's valid part: xy its corner (world), zw its size
  deformPatch: vec4f,       // the patch: xy its centre, z half its size, w the band it hands over in
  deformGround: vec4f,      // the ground under the patch: xy its corner (world), z its texel
}

@group(1) @binding(0) var<uniform> terrain: Terrain;
@group(1) @binding(1) var splatMap: texture_2d<f32>;    // rgba layers 2-5's weights (data)
@group(1) @binding(2) var splatSampler: sampler;        // smooth, clamped at the edges
@group(1) @binding(3) var colorLayers: texture_2d_array<f32>;  // sRGB: rgb colour, a height
@group(1) @binding(4) var detailLayers: texture_2d_array<f32>; // data: rg normal xy, b roughness
@group(1) @binding(5) var layerSampler: sampler;        // repeating, mipmapped
@group(1) @binding(6) var tintMap: texture_2d<f32>;     // sRGB: x the colour, over the mesh's UVs
@group(1) @binding(7) var puddleMap: texture_2d<f32>;   // data: r level, g depth, b fill rate (see puddleImage)
@group(1) @binding(8) var deformMap: texture_2d<f32>;   // data: r the ground's offset (m), wrapping
@group(1) @binding(9) var deformGround: texture_2d<f32>; // data: height, uv, 1 on the mesh (loaded)
@group(1) @binding(10) var paintMap: texture_2d<f32>;   // data: rgba layers 2-5's weights a game paints

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

// Where a game's paint (or tint) `size` texels across is read for a point at
// `uv`: wobbled by up to about a texel, in a pattern fixed to the texture
// (so to the ground: it holds still as the camera moves), so its texels
// never show as squares, nor the edge of what it paints as a straight line.
fn paintedUv(uv: vec2f, size: vec2f) -> vec2f {
  let t = uv * size;
  let wobble = vec2f(terrainNoise(t * 0.33 + 3.1), terrainNoise(t * 0.33 + 17.7)) - 0.5;
  let fine = vec2f(terrainNoise(t * 0.9 + 41.3), terrainNoise(t * 0.9 + 9.2)) - 0.5;
  return uv + (wobble * 1.7 + fine * 0.7) / size;
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

// Grass seen from afar, past where its blades are drawn (a layer's meadow;
// where grass grows on the terrain, its ground carries the grass's own look
// over these layers instead, see meshGrass.wgsl's fs_ground):
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

// Puddles where the weather's puddles (0..1) have filled its low spots, x
// how readily water pools on the layers there (`pooling`). The puddle map
// (PuddleModule's, see puddles.h) holds for each texel how far up its
// hollow's water line it is (r: 0 its lowest point, 1 the line or dry) and
// how deep the hollow's water is at the line (g): water stands where the
// line, risen to the weather's puddles x how fast it fills there (b, the
// game's fill rate: loose soil soaks it up), is above it. x the water (0..1,
// a narrow soft edge), y how deep it is (m), z how soaked the ground is
// just above its edge (a darker wet rim).
fn terrainPuddles(texel: vec4f, pooling: f32) -> vec3f {
  let line = frame.weather.z * pooling * texel.b;
  if (terrain.puddleParams.x < 0.5 || line <= 0.0) {
    return vec3f(0.0);
  }
  let level = texel.r;
  let water = clamp((line * 0.98 - level) / 0.035, 0.0, 1.0);
  let depth = texel.g * terrain.puddleParams.y * max(line - level, 0.0);
  let rim = clamp((line + 0.18 - level) / 0.18, 0.0, 1.0) * step(level, 0.999);
  return vec3f(water, depth, rim);
}

// --- Deformation (see deformation.h and DeformationModule) ---------------
//
// A window of the deformation map around the camera (texel t of the world at
// t mod its size: it wraps, so moving it only writes what comes into it)
// says how far the ground is pressed in or pushed up. Near the camera a fine
// patch mesh drawn with this surface (deformMap.x 2) is displaced by it, and
// the ground's own mesh is cut away under it; out to the window's edge the
// ground is parallax-mapped. Over a band inside the patch's edge its
// displacement fades out as parallax takes over, so there's no line.

// How much of the map shows at a world point: all of it inside its window,
// fading out over its last 3 m.
fn deformFade(p: vec2f) -> f32 {
  let local = p - terrain.deformWindow.xy;
  let size = terrain.deformWindow.zw;
  let edge = min(min(local.x, local.y), min(size.x - local.x, size.y - local.y));
  return clamp(edge / 3.0, 0.0, 1.0);
}

// The ground's offset at a world point (m; negative where it's pressed in).
fn deformAt(p: vec2f) -> f32 {
  return textureSampleLevel(deformMap, layerSampler, p * terrain.deformMap.z, 0.0).r * deformFade(p);
}

// The offset smoothed over about `spacing` (m): what a patch vertex that far
// from its neighbours shows, so detail finer than its triangles doesn't
// alias into spikes (its normals, per pixel, show that).
fn deformAtSmoothed(p: vec2f, spacing: f32) -> f32 {
  if (spacing < terrain.deformMap.y * 1.5) {
    return deformAt(p);
  }
  let r = spacing / 3.0;
  var sum = 0.0;
  for (var y = -1; y <= 1; y++) {
    for (var x = -1; x <= 1; x++) {
      sum += deformAt(p + vec2f(f32(x), f32(y)) * r);
    }
  }
  return sum / 9.0;
}

// How much of the offset the patch's geometry shows at a point: all of it,
// fading out over the band inside its edge.
fn deformPatchWeight(p: vec2f) -> f32 {
  let away = max(abs(p.x - terrain.deformPatch.x), abs(p.y - terrain.deformPatch.y));
  return 1.0 - smoothstep(terrain.deformPatch.z - terrain.deformPatch.w, terrain.deformPatch.z, away);
}

// The ground under the patch at a point (bilinear): height, uv, and 1 on
// the mesh; and its slope (the height's rise per metre along x and y).
struct DeformGround {
  texel: vec4f,
  slope: vec2f,
}

fn deformGroundAt(p: vec2f) -> DeformGround {
  let size = vec2i(textureDimensions(deformGround));
  let f = (p - terrain.deformGround.xy) / terrain.deformGround.z - 0.5;
  let i = vec2i(floor(f));
  let t = f - floor(f);
  let a = textureLoad(deformGround, clamp(i, vec2i(0), size - 1), 0);
  let b = textureLoad(deformGround, clamp(i + vec2i(1, 0), vec2i(0), size - 1), 0);
  let c = textureLoad(deformGround, clamp(i + vec2i(0, 1), vec2i(0), size - 1), 0);
  let d = textureLoad(deformGround, clamp(i + vec2i(1, 1), vec2i(0), size - 1), 0);
  var out: DeformGround;
  out.texel = mix(mix(a, b, t.x), mix(c, d, t.x), t.y);
  out.slope = vec2f(mix(b.x - a.x, d.x - c.x, t.y), mix(c.x - a.x, d.x - b.x, t.x)) / terrain.deformGround.z;
  return out;
}

// The patch's vertices: on the ground under them, pressed in. Its mesh is
// flat rings around the camera (see DeformationModule): uv is the way to a
// vertex's neighbours along a coarser ring's edge, if it's between two
// (it's put halfway, so the rings meet without cracks), the normal's length
// the spacing the map's smoothed over for it, and a downward normal marks
// its skirt (hung under its edge).
fn surfaceVertex(v: SurfaceVertex) -> SurfaceVertex {
  if (terrain.deformMap.x < 1.5) {
    return v;
  }
  let p = v.position.xy;
  let spacing = abs(v.normal.z);
  let ground = deformGroundAt(p);
  var height = ground.texel.x + deformAtSmoothed(p, spacing) * deformPatchWeight(p);
  if (dot(v.uv, v.uv) > 0.0) {
    let a = p + v.uv;
    let b = p - v.uv;
    height = 0.5 * (deformGroundAt(a).texel.x + deformAtSmoothed(a, spacing) * deformPatchWeight(a) +
                    deformGroundAt(b).texel.x + deformAtSmoothed(b, spacing) * deformPatchWeight(b));
  }
  // The ground's own slope (the pressed ground's is per pixel: terrainDeformed).
  var out = v;
  out.position = vec3f(p, height - select(0.0, 0.25, v.normal.z < 0.0));
  out.normal = normalize(vec3f(-ground.slope, 1.0));
  out.uv = ground.texel.yz;
  out.keep = step(0.5, ground.texel.w);
  return out;
}

// Where it's deformed: the point the eye sees, the pressed ground's slope (a
// nudge to the normal along the world's axes), and whether it's drawn here
// at all (0: the patch is, over the ground's own mesh).
struct Deformed {
  position: vec3f,
  tilt: vec3f,
  keep: f32,
}

fn terrainDeformed(in: SurfaceIn) -> Deformed {
  var out: Deformed;
  out.position = in.position;
  out.tilt = vec3f(0.0);
  out.keep = 1.0;
  let mode = terrain.deformMap.x;
  if (mode > 0.5) {
    let p = in.position;
    let pixel = max(length(dpdx(p)), length(dpdy(p)));
    let away = max(abs(p.x - terrain.deformPatch.x), abs(p.y - terrain.deformPatch.y));
    if (mode < 1.5 && away < terrain.deformPatch.z) {
      out.keep = 0.0;
    }
    // How much of the offset this surface already shows (the patch's
    // geometry), and the rest followed down by parallax: the ray from the eye
    // on below the surface drawn, in a few steps, to where it meets the
    // pressed ground.
    let shown = select(0.0, deformPatchWeight(p.xy), mode > 1.5);
    let d0 = deformAt(p.xy);
    var hit = p;
    let ray = -in.toView;
    let down = -dot(ray, in.normal);
    if (d0 < -0.001 && shown < 0.999 && out.keep > 0.5 && down > 0.02) {
      let top = shown * d0;
      let bottom = min(terrain.deformMap.w, d0) - 0.005;
      let reach = min((top - bottom) / down, 3.0);
      var t0 = 0.0;
      var gap0 = top - d0;
      var found = false;
      var at = reach;
      for (var i = 1; i <= 12; i++) {
        let t = reach * f32(i) / 12.0;
        let q = p + ray * t;
        let gap = top - down * t - deformAt(q.xy);
        if (!found && gap <= 0.0) {
          at = t0 + (t - t0) * gap0 / max(gap0 - gap, 1e-6);
          found = true;
        }
        if (!found) {
          t0 = t;
          gap0 = gap;
        }
      }
      hit = p + ray * at;
    }
    out.position = hit;
    // Its slope, across a texel (or a pixel, further off, so it doesn't
    // shimmer).
    let e = max(terrain.deformMap.y, pixel);
    let slope = vec2f(deformAt(hit.xy + vec2f(e, 0.0)) - deformAt(hit.xy - vec2f(e, 0.0)),
                      deformAt(hit.xy + vec2f(0.0, e)) - deformAt(hit.xy - vec2f(0.0, e)));
    out.tilt = vec3f(-slope / (2.0 * e), 0.0);
  }
  return out;
}

fn surface(in: SurfaceIn) -> Surface {
  // Where it's pressed in, the point the eye sees there (first: it may
  // branch, but all of it reconverges before the textures below are
  // sampled).
  let deformed = terrainDeformed(in);
  // In uniform control flow: the splat map, and the derivatives every
  // layer's sampling uses (so the layers can be skipped where they're not).
  let splat = textureSample(splatMap, splatSampler, in.uv);
  let tintTexel = textureSample(tintMap, splatSampler, paintedUv(in.uv, vec2f(textureDimensions(tintMap))));
  let tint = tintTexel.rgb;
  let paint = textureSample(paintMap, splatSampler, paintedUv(in.uv, vec2f(textureDimensions(paintMap))));
  let world = deformed.position.xy;
  // Its puddle map over the world's XY (v down the image: its +y up it).
  let puddleAt = (world - terrain.puddleMap.xy) * terrain.puddleMap.zw;
  let puddleTexel = textureSample(puddleMap, splatSampler, vec2f(puddleAt.x, 1.0 - puddleAt.y));
  let dx3 = dpdx(in.position);
  let dy3 = dpdy(in.position);
  let dx = dx3.xy;
  let dy = dy3.xy;
  // By the pressed ground's own slope too: a rut's steep walls are laid from
  // the side (triplanar layers), not stretched down them.
  let pressedNormal = normalize(in.normal + deformed.tilt - in.normal * dot(in.normal, deformed.tilt));
  let projections = terrainProjections(pressedNormal);

  // Each layer's weight: the splat map's four (giving way to what the game
  // paints over them) over the first, sharing past 1.
  let byGame = clamp(paint, vec4f(0.0), vec4f(1.0));
  let given = min(byGame.x + byGame.y + byGame.z + byGame.w, 1.0);
  let painted = clamp(splat, vec4f(0.0), vec4f(1.0)) * (1.0 - given) + byGame;
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
      samples[i] = terrainLayerAt(layer, deformed.position, dx3, dy3, projections);
      tallest = max(tallest, samples[i].color.a + weights[i]);
    }
  }
  let band = mix(1.0, 0.04, clamp(terrain.params.y, 0.0, 1.0));
  var color = vec3f(0.0);
  var tilt = vec3f(0.0);
  var roughness = 0.0;
  var meadow = 0.0;
  var pooling = 0.0;
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
    pooling += terrain.puddleLayers[layer / 4][layer % 4] * blend;
    total += blend;
  }
  let share = 1.0 / max(total, 1e-5);
  color *= share;
  tilt *= share;
  roughness *= share;
  meadow *= share;
  pooling *= share;

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
  tilt += deformed.tilt;
  let bent = normalize(up + tilt - up * dot(up, tilt));

  // Under a puddle the ground shows through muddy water, browner and
  // murkier the deeper it is (the lit pass makes it smooth, wet and rippled).
  let puddle = terrainPuddles(puddleTexel, pooling);
  if (puddle.x > 0.0) {
    let murk = (1.0 - exp(-puddle.y / 0.025)) * puddle.x;
    let mud = vec3f(0.30, 0.22, 0.14) * dot(color, vec3f(0.3, 0.59, 0.11)) * 1.6;
    color = mix(color, mud, murk * 0.75);
  }

  var result: Surface;
  result.baseColor = color;
  result.alpha = deformed.keep;
  result.metallic = 0.0;
  result.roughness = clamp(roughness, 0.04, 1.0);
  result.normal = normalize(vec3f(dot(bent, in.tangent), dot(bent, in.bitangent), dot(bent, up)));
  result.emission = vec3f(0.0);
  result.alphaClip = 0.5;
  result.bloom = 1.0;
  result.water = puddle.x;
  // Soaked by the puddles' rims, and where the tint says the ground's wet:
  // the more so down in a rut (its walls and floor wet mud).
  let pressedIn = select(0.0, clamp(-deformAt(world) * 5.0, 0.0, 1.0), terrain.deformMap.x > 0.5);
  result.soak = max(puddle.z, min((1.0 - tintTexel.a) * (1.0 + 0.4 * pressedIn), 1.0));
  return result;
}

// Only whether it's drawn here, for the passes that need nothing else
// (shadows, depth): cut away under the patch, as surface() has it.
fn surfaceAlpha(in: SurfaceIn) -> vec2f {
  let away = max(abs(in.position.x - terrain.deformPatch.x), abs(in.position.y - terrain.deformPatch.y));
  let cut = terrain.deformMap.x > 0.5 && terrain.deformMap.x < 1.5 && away < terrain.deformPatch.z;
  return vec2f(select(1.0, 0.0, cut), 0.5);
}

// Deformed, cut out where the patch is drawn over it (and the patch where
// it's off the mesh).
fn surfaceAlphaMode() -> u32 {
  return select(ALPHA_OPAQUE, ALPHA_MASK, terrain.deformMap.x > 0.5);
}
