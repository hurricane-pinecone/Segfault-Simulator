// Screen-space ambient occlusion's full-screen passes, at half the viewport's
// size, after meshDepthNormals.wgsl has drawn what's opaque (its normals and
// depth): fs_occlusion works out how open each point is to the light from all
// around, then fs_blur smooths that across and then down, along surfaces
// but not over their edges (by depth). The lit pass reads the result (r how
// open, g the depth), weighting its texels by depth again (meshLit.wgsl's
// ambientOcclusionAt). Must match OcclusionUniforms in framePlan.h.

struct Occlusion {
  inverseViewProjection: mat4x4f,
  camera: vec4f,  // xyz where it is, w 1 for a perspective view (0: orthographic)
  forward: vec4f, // xyz which way it looks
  params: vec4f,  // x radius (world), y intensity, z samples, w pixels a world unit spans
                  // at depth 1 (orthographic: at any)
  size: vec4f,    // xy this pass's size (texels), zw 1 / that
  blur: vec4f,    // xy a blur's step (texels), z where it starts fading with distance, w
                  // where it's gone
}

@group(0) @binding(0) var<uniform> occlusion: Occlusion;
// fs_occlusion: the normals (xyz) and depth (w); fs_blur: the occlusion (r)
// and depth (g).
@group(0) @binding(1) var source: texture_2d<f32>;

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4f {
  let corner = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
  return vec4f(corner * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
}

// The world position of what's at texel coordinates `at` (this pass's), its
// depth along the view `depth`.
fn positionAt(at: vec2f, depth: f32) -> vec3f {
  let ndc = vec2f(at.x * occlusion.size.z * 2.0 - 1.0, 1.0 - at.y * occlusion.size.w * 2.0);
  let near = occlusion.inverseViewProjection * vec4f(ndc, 0.0, 1.0);
  let far = occlusion.inverseViewProjection * vec4f(ndc, 1.0, 1.0);
  let origin = near.xyz / near.w;
  let ray = normalize(far.xyz / far.w - origin);
  let forward = occlusion.forward.xyz;
  let along = (depth - dot(origin - occlusion.camera.xyz, forward)) / max(dot(ray, forward), 1e-4);
  return origin + ray * along;
}

// How open a point is: points around it on screen (a spiral, turned a
// little differently for each pixel), each blocking it as far as it rises
// above its surface (the cosine to its normal), less the farther off it is,
// none past the radius.
@fragment
fn fs_occlusion(@builtin(position) position: vec4f) -> @location(0) vec4f {
  let pixel = vec2i(position.xy);
  let here = textureLoad(source, pixel, 0);
  let depth = here.w;
  if (depth <= 0.0) {
    return vec4f(1.0, 0.0, 0.0, 1.0); // nothing drawn: open sky
  }
  let fade = smoothstep(occlusion.blur.z, occlusion.blur.w, depth);
  let radius = occlusion.params.x;
  let perUnit = select(occlusion.params.w, occlusion.params.w / depth, occlusion.camera.w > 0.5);
  let reach = min(radius * perUnit, 48.0);
  if (reach < 1.0 || fade >= 1.0) {
    return vec4f(1.0, depth, 0.0, 1.0);
  }

  let normal = normalize(here.xyz);
  let center = positionAt(position.xy, depth);
  let samples = max(u32(occlusion.params.z), 1u);
  // Interleaved gradient noise: a different turn for each pixel, which the
  // blur evens out.
  let noise = fract(52.9829189 * fract(dot(position.xy, vec2f(0.06711056, 0.00583715))));
  let size = vec2i(occlusion.size.xy);
  let radius2 = radius * radius;
  var blocked = 0.0;
  for (var i = 0u; i < samples; i++) {
    let t = (f32(i) + 0.5) / f32(samples);
    let angle = (t * 7.0 + noise) * 6.2831853;
    let at = vec2i(floor(position.xy + vec2f(cos(angle), sin(angle)) * (reach * t)));
    if (any(at < vec2i(0)) || any(at >= size) || all(at == pixel)) {
      continue;
    }
    let other = textureLoad(source, at, 0);
    if (other.w <= 0.0) {
      continue;
    }
    let toward = positionAt(vec2f(at) + vec2f(0.5), other.w) - center;
    let distance2 = dot(toward, toward);
    let rise = dot(toward, normal) * inverseSqrt(distance2 + 1e-6);
    blocked += max(rise - 0.1, 0.0) * max(1.0 - distance2 / radius2, 0.0);
  }
  let openness = clamp(1.0 - occlusion.params.y * 2.2 * blocked / f32(samples), 0.0, 1.0);
  return vec4f(mix(openness, 1.0, fade), depth, 0.0, 1.0);
}

// Gaussian along blur.xy, each texel weighted by how near its depth is to
// this one's (so occlusion doesn't bleed across an edge onto what's behind).
@fragment
fn fs_blur(@builtin(position) position: vec4f) -> @location(0) vec4f {
  let pixel = vec2i(position.xy);
  let here = textureLoad(source, pixel, 0);
  if (here.g <= 0.0) {
    return here;
  }
  let stride = vec2i(occlusion.blur.xy);
  let size = vec2i(occlusion.size.xy);
  let tolerance = here.g * 0.04 + 0.02;
  var sum = here.r;
  var total = 1.0;
  for (var i = 1; i <= 4; i++) {
    let gaussian = exp(-f32(i * i) / 8.0);
    for (var side = -1; side <= 1; side += 2) {
      let at = clamp(pixel + stride * (i * side), vec2i(0), size - vec2i(1));
      let other = textureLoad(source, at, 0);
      let weight = gaussian * max(1.0 - abs(other.g - here.g) / tolerance, 0.0) * select(0.0, 1.0, other.g > 0.0);
      sum += other.r * weight;
      total += weight;
    }
  }
  return vec4f(sum / total, here.g, 0.0, 1.0);
}
