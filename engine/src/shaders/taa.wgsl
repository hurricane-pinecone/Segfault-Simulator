// Temporal anti-aliasing (FrameParams::temporalAntiAliasing), after the lit
// pass: the camera is jittered a fraction of a pixel each frame (a different
// fraction each time), so blending this frame's picture into what came before
// samples each pixel all over its area, smoothing edges and the shimmer of
// fine detail.
//
// For each pixel, where it was last frame: its point in the world (from the
// depth buffer, through this frame's camera, not jittered) seen through last
// frame's (nor that), so a still camera finds it exactly where it is. There,
// the history (the last frame's blend) is clamped to the colours around this
// pixel now, so it can't keep what's no longer there (what moved, or came out
// from behind something) and smear; then a little
// of this frame is mixed in, and the result sharpened a little against the
// softening the blend of jittered frames brings. Off the screen last frame, or
// with no history yet, it's this frame alone.

struct Taa {
  inverseViewProjection: mat4x4f,  // this frame's (not jittered): clip to world
  previousViewProjection: mat4x4f, // last frame's (not jittered)
  params: vec4f, // xy 1 / the size (texels), z history valid (0/1), w how much of this frame
}

@group(0) @binding(0) var current: texture_2d<f32>;
@group(0) @binding(1) var history: texture_2d<f32>;
@group(0) @binding(2) var depthMap: texture_depth_multisampled_2d;
@group(0) @binding(3) var linearClamp: sampler;
@group(0) @binding(4) var<uniform> taa: Taa;

struct Varyings {
  @builtin(position) position: vec4f,
  @location(0) uv: vec2f,
}

// How much the blend's result is sharpened.
const kSharpen: f32 = 0.25;

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> Varyings {
  let corner = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
  var out: Varyings;
  out.position = vec4f(corner * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
  out.uv = corner;
  return out;
}

// The history at `uv`, Catmull-Rom filtered (sharper than bilinear, so it
// doesn't soften as it's carried along a moving camera): its 4 x 4 texels
// from 5 bilinear taps, the corners left out.
fn historyAt(uv: vec2f) -> vec3f {
  let size = vec2f(textureDimensions(history));
  let at = uv * size;
  let centre = floor(at - 0.5) + 0.5;
  let f = at - centre;
  let w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
  let w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
  let w2 = f * (0.5 + f * (2.0 - 1.5 * f));
  let w3 = f * f * (-0.5 + 0.5 * f);
  let w12 = w1 + w2;
  let at0 = (centre - 1.0) / size;
  let at3 = (centre + 2.0) / size;
  let at12 = (centre + w2 / w12) / size;
  var c = textureSampleLevel(history, linearClamp, vec2f(at12.x, at0.y), 0.0).rgb * (w12.x * w0.y);
  c += textureSampleLevel(history, linearClamp, vec2f(at0.x, at12.y), 0.0).rgb * (w0.x * w12.y);
  c += textureSampleLevel(history, linearClamp, at12, 0.0).rgb * (w12.x * w12.y);
  c += textureSampleLevel(history, linearClamp, vec2f(at3.x, at12.y), 0.0).rgb * (w3.x * w12.y);
  c += textureSampleLevel(history, linearClamp, vec2f(at12.x, at3.y), 0.0).rgb * (w12.x * w3.y);
  let total = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
  return max(c / total, vec3f(0.0));
}

// Brightness, for weighting: bright pixels count for less, so a lone bright
// sample (a glint) doesn't flicker through the blend.
fn weightOf(c: vec3f) -> f32 {
  return 1.0 / (1.0 + dot(c, vec3f(0.2126, 0.7152, 0.0722)));
}

@fragment
fn fs_resolve(in: Varyings) -> @location(0) vec4f {
  let pixel = vec2i(in.position.xy);
  let size = vec2i(textureDimensions(current));
  let now = textureLoad(current, pixel, 0).rgb;

  // The colours around it now: what the history may hold.
  var low = now;
  var high = now;
  var around = vec3f(0.0);
  for (var y = -1; y <= 1; y++) {
    for (var x = -1; x <= 1; x++) {
      let c = textureLoad(current, clamp(pixel + vec2i(x, y), vec2i(0), size - 1), 0).rgb;
      low = min(low, c);
      high = max(high, c);
      around += c;
    }
  }
  around /= 9.0;

  // Where it was last frame.
  let depth = textureLoad(depthMap, pixel, 0);
  let ndc = vec2f(in.uv.x * 2.0 - 1.0, 1.0 - in.uv.y * 2.0);
  let world = taa.inverseViewProjection * vec4f(ndc, depth, 1.0);
  let before = taa.previousViewProjection * vec4f(world.xyz / world.w, 1.0);
  let beforeNdc = before.xy / before.w;
  let beforeUv = vec2f(beforeNdc.x * 0.5 + 0.5, 0.5 - beforeNdc.y * 0.5);
  let onScreen = all(beforeUv >= vec2f(0.0)) && all(beforeUv <= vec2f(1.0)) && before.w > 0.0;
  if (taa.params.z < 0.5 || !onScreen) {
    return vec4f(now, 1.0);
  }

  let past = clamp(historyAt(beforeUv), low, high);
  let share = taa.params.w;
  let a = (1.0 - share) * weightOf(past);
  let b = share * weightOf(now);
  let blended = (past * a + now * b) / (a + b);
  // A little sharper (unsharp: away from the neighbourhood's average), kept
  // within what's around it so edges don't ring.
  return vec4f(clamp(blended + (blended - around) * kSharpen, low, high), 1.0);
}
