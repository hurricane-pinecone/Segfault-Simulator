// Depth of field (DepthOfFieldSettings), between the lit pass and bloom:
// what's nearer or farther than the focus blurs, as through a real lens.
//
// fs_prepare halves the scene, with each pixel's blur (0 sharp to 1 fully
// blurred, from the depth buffer) beside it in alpha; fs_blur gathers a disc
// round each of those, as wide as its blur, from the taps whose own blur
// reaches it (so sharp things don't smear over blurred ones); fs_combine mixes
// that over the full-size scene by each pixel's blur. Each draws one
// full-screen triangle.

struct Focus {
  depth: vec4f, // x projection[2][2], y projection[3][2], z 1 perspective (0 orthographic)
  focus: vec4f, // x focus distance, y half the sharp range, z blur radius (half-size texels) at full blur
  texel: vec4f, // xy a half-size texel (uv)
}

@group(0) @binding(0) var source: texture_2d<f32>; // the scene, or (fs_blur) its half
@group(0) @binding(1) var linearClamp: sampler;
@group(0) @binding(2) var<uniform> focus: Focus;
@group(0) @binding(3) var depthMap: texture_depth_multisampled_2d; // the lit pass's
@group(0) @binding(4) var blurred: texture_2d<f32>; // fs_combine's

struct Varyings {
  @builtin(position) position: vec4f,
  @location(0) uv: vec2f, // (0, 0) at the top left
}

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> Varyings {
  // One triangle covering the screen.
  let corner = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
  var out: Varyings;
  out.position = vec4f(corner * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
  out.uv = corner;
  return out;
}

// How blurred a full-size pixel is, 0 to 1: nothing within the sharp range,
// easing in beyond it, fully one and a half focus distances past it (and
// toward the camera, as much).
fn blurAt(pixel: vec2i) -> f32 {
  let size = vec2i(textureDimensions(depthMap));
  let d = textureLoad(depthMap, clamp(pixel, vec2i(0), size - 1), 0); // its first sample
  let away = select(-(d - focus.depth.y) / focus.depth.x,
                    focus.depth.y / min(d + focus.depth.x, -1e-6),
                    focus.depth.z > 0.5);
  let outside = max(abs(away - focus.focus.x) - focus.focus.y, 0.0);
  return smoothstep(0.0, 1.0, outside / max(focus.focus.x * 1.5, 1.0));
}

@fragment
fn fs_prepare(in: Varyings) -> @location(0) vec4f {
  let color = textureSampleLevel(source, linearClamp, in.uv, 0.0).rgb;
  let pixel = vec2i(in.position.xy) * 2;
  let blur = (blurAt(pixel) + blurAt(pixel + vec2i(1, 0)) + blurAt(pixel + vec2i(0, 1)) +
              blurAt(pixel + vec2i(1, 1))) * 0.25;
  return vec4f(color, blur);
}

const TAPS: i32 = 32;

@fragment
fn fs_blur(in: Varyings) -> @location(0) vec4f {
  let center = textureSampleLevel(source, linearClamp, in.uv, 0.0);
  let radius = center.a * focus.focus.z;
  if (radius < 0.5) {
    return center;
  }
  // A golden-angle spiral of taps over the disc, evenly spread.
  var sum = center.rgb;
  var total = 1.0;
  for (var i = 0; i < TAPS; i++) {
    let r = sqrt((f32(i) + 0.5) / f32(TAPS)) * radius;
    let angle = f32(i) * 2.39996323;
    let tap = textureSampleLevel(source, linearClamp,
                                 in.uv + vec2f(cos(angle), sin(angle)) * r * focus.texel.xy, 0.0);
    // Only taps blurred enough to reach here.
    let weight = clamp(tap.a * focus.focus.z - r + 1.0, 0.0, 1.0);
    sum += tap.rgb * weight;
    total += weight;
  }
  return vec4f(sum / total, center.a);
}

@fragment
fn fs_combine(in: Varyings) -> @location(0) vec4f {
  let pixel = vec2i(in.position.xy);
  let sharp = textureLoad(source, pixel, 0).rgb;
  let soft = textureSampleLevel(blurred, linearClamp, in.uv, 0.0).rgb;
  // Fading into the blurred picture gradually, fully at half blur (the
  // half-size picture is softer than the scene even barely blurred, so a
  // quick switch shows as a line).
  let amount = smoothstep(0.0, 0.5, blurAt(pixel));
  return vec4f(mix(sharp, soft, amount), 1.0);
}
