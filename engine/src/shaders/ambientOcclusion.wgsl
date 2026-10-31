// Screen-space ambient occlusion's full-screen passes, at half the viewport's
// size, after meshDepthNormals.wgsl has drawn what's opaque (its normals and
// depth): fs_occlusion works out how open each point is to the light from all
// around, and (with contact shadows on) how much of the sun's light reaches it
// past what's just in front of it, then fs_blur smooths those across and then
// down, along surfaces but not over their edges (by depth). The lit pass reads
// the result (r how open, g the depth, b how much of the sun's light), weighting
// its texels by depth again (meshLit.wgsl's screenOcclusionAt). Must match
// OcclusionUniforms in framePlan.h.

struct Occlusion {
  inverseViewProjection: mat4x4f,
  viewProjection: mat4x4f, // contact shadows' rays onto the screen
  camera: vec4f,  // xyz where it is, w 1 for a perspective view (0: orthographic)
  forward: vec4f, // xyz which way it looks
  params: vec4f,  // x radius (world), y intensity, z samples, w pixels a world unit spans
                  // at depth 1 (orthographic: at any)
  size: vec4f,    // xy this pass's size (texels), zw 1 / that
  blur: vec4f,    // xy a blur's step (texels), z where it starts fading with distance, w
                  // where it's gone
  light: vec4f,   // xyz toward the frame's light (sun or moon), w ambient occlusion on (0/1)
  contact: vec4f, // contact shadows: x length, y thickness (world), z strength (0: none),
                  // w where they've faded out with distance
}

@group(0) @binding(0) var<uniform> occlusion: Occlusion;
// fs_occlusion: the normals (xyz) and depth (w); fs_blur: the occlusion (r),
// depth (g) and the sun's light (b).
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

// World units a texel spans at `depth`.
fn texelWorld(depth: f32) -> f32 {
  return select(1.0, depth, occlusion.camera.w > 0.5) / max(occlusion.params.w, 1e-4);
}

@fragment
fn fs_occlusion(@builtin(position) position: vec4f) -> @location(0) vec4f {
  let pixel = vec2i(position.xy);
  let here = textureLoad(source, pixel, 0);
  let depth = here.w;
  if (depth <= 0.0) {
    return vec4f(1.0, 0.0, 1.0, 1.0); // nothing drawn: open sky, in the sun
  }
  let normal = normalize(here.xyz);
  let center = positionAt(position.xy, depth);
  // Interleaved gradient noise: a different turn (and start) for each pixel,
  // which the blur evens out.
  let noise = fract(52.9829189 * fract(dot(position.xy, vec2f(0.06711056, 0.00583715))));
  var open = 1.0;
  if (occlusion.light.w > 0.5) {
    open = openness(position.xy, pixel, depth, normal, center, noise);
  }
  var sunlit = 1.0;
  if (occlusion.contact.z > 0.0) {
    sunlit = 1.0 - contactShadow(pixel, depth, normal, center, noise) * occlusion.contact.z;
  }
  return vec4f(open, depth, sunlit, 1.0);
}

// How open a point is: points around it on screen (a spiral, turned a
// little differently for each pixel), each blocking it as far as it rises
// above its surface (the cosine to its normal), less the farther off it is,
// none past the radius.
fn openness(position: vec2f, pixel: vec2i, depth: f32, normal: vec3f, center: vec3f, noise: f32) -> f32 {
  let fade = smoothstep(occlusion.blur.z, occlusion.blur.w, depth);
  let radius = occlusion.params.x;
  let perUnit = select(occlusion.params.w, occlusion.params.w / depth, occlusion.camera.w > 0.5);
  let reach = min(radius * perUnit, 48.0);
  if (reach < 1.0 || fade >= 1.0) {
    return 1.0;
  }

  let samples = max(u32(occlusion.params.z), 1u);
  let size = vec2i(occlusion.size.xy);
  let radius2 = radius * radius;
  var blocked = 0.0;
  for (var i = 0u; i < samples; i++) {
    let t = (f32(i) + 0.5) / f32(samples);
    let angle = (t * 7.0 + noise) * 6.2831853;
    let at = vec2i(floor(position + vec2f(cos(angle), sin(angle)) * (reach * t)));
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
  let open = clamp(1.0 - occlusion.params.y * 2.2 * blocked / f32(samples), 0.0, 1.0);
  return mix(open, 1.0, fade);
}

// How much of the frame's light a point is kept from by what's just in front
// of it (0 none, 1 all): a short ray toward the light, from a little off the
// surface (and a little along, differently for each pixel), stepped over the
// screen until something drawn is in front of it (but not by more than the
// thickness: it's passing behind that) or it leaves the screen. What's in
// front is measured from that texel's surface (its plane, by its normal)
// where the ray crosses it, not from the texel's middle, so a slanted ground
// doesn't shade itself. Less toward the ray's end, the screen's edges, and
// with distance.
fn contactShadow(pixel: vec2i, depth: f32, normal: vec3f, center: vec3f, noise: f32) -> f32 {
  let toLight = occlusion.light.xyz;
  let fade = smoothstep(occlusion.contact.w * 0.6, occlusion.contact.w, depth);
  // Facing away, it has none of the light to lose; nor once the light's set.
  if (fade >= 1.0 || dot(normal, toLight) < 0.02 || toLight.z < 0.0) {
    return 0.0;
  }
  let texel = texelWorld(depth);
  let size = occlusion.size.xy;
  let rayLength = occlusion.contact.x;
  let thickness = occlusion.contact.y;
  let forward = occlusion.forward.xyz;
  let perspective = occlusion.camera.w > 0.5;
  let start = center + normal * (texel * 0.75 + 0.01);
  let steps = 12u;
  for (var i = 0u; i < steps; i++) {
    let along = (f32(i) + noise) / f32(steps);
    let onRay = start + toLight * (rayLength * along);
    let clip = occlusion.viewProjection * vec4f(onRay, 1.0);
    if (clip.w <= 1e-4) {
      break;
    }
    let ndc = clip.xy / clip.w;
    let at = vec2f(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * size;
    if (any(at < vec2f(0.0)) || any(at >= size)) {
      break;
    }
    let other = vec2i(floor(at));
    if (all(other == pixel)) {
      continue;
    }
    let there = textureLoad(source, other, 0);
    if (there.w <= 0.0) {
      continue;
    }
    // How far behind what's drawn there the ray is: by that texel's plane,
    // along the view through the ray's point, unless it's seen edge on.
    let pointDepth = dot(onRay - occlusion.camera.xyz, forward);
    let viewRay = select(forward, normalize(onRay - occlusion.camera.xyz), perspective);
    let surfaceNormal = normalize(there.xyz);
    let facing = dot(viewRay, surfaceNormal);
    var behind = pointDepth - there.w;
    if (facing < -0.2) {
      let plane = positionAt(vec2f(other) + vec2f(0.5), there.w);
      behind = dot(onRay - plane, surfaceNormal) * dot(viewRay, forward) / facing;
    }
    if (behind > 0.01 + texel * 0.1 && behind < thickness) {
      let edge = clamp(min(min(at.x, size.x - at.x), min(at.y, size.y - at.y)) / 8.0, 0.0, 1.0);
      return (1.0 - smoothstep(0.6, 1.0, along)) * edge * (1.0 - fade);
    }
  }
  return 0.0;
}

// Gaussian along blur.xy, each texel weighted by how near its depth is to
// this one's (so occlusion doesn't bleed across an edge onto what's behind);
// the sun's light over a narrower one, keeping contact shadows sharp.
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
  var sum = vec2f(here.r, here.b);
  var total = vec2f(1.0);
  for (var i = 1; i <= 4; i++) {
    let gaussian = vec2f(exp(-f32(i * i) / 8.0), exp(-f32(i * i) / 1.5));
    for (var side = -1; side <= 1; side += 2) {
      let at = clamp(pixel + stride * (i * side), vec2i(0), size - vec2i(1));
      let other = textureLoad(source, at, 0);
      let weight = gaussian * max(1.0 - abs(other.g - here.g) / tolerance, 0.0) * select(0.0, 1.0, other.g > 0.0);
      sum += vec2f(other.r, other.b) * weight;
      total += weight;
    }
  }
  return vec4f(sum.x / total.x, here.g, sum.y / total.y, 1.0);
}
