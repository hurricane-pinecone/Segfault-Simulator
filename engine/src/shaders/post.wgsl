// The renderer's passes after the lit scene: bloom, then the composite
// into the target. Each draws one full-screen triangle.
//
// Bloom: the prefilter keeps what of the light that blooms (the scene x each
// material's Bloom, which the lit pass writes beside it) is brighter than the
// threshold, while halving it; downsamples halve it again and again (a 13-tap
// filter, so the result doesn't shimmer); upsamples then add each level onto the one above
// it, spread by a tent filter, back up to the first. The composite adds that
// glow to the scene and clamps to 0..1 (lit surfaces never went above 1, so
// without bloom the picture is as it always was).

struct Post {
  texel: vec4f, // xy the source's texel size
  bloom: vec4f, // x threshold, y softness, z radius (texels), w intensity
}

@group(0) @binding(0) var source: texture_2d<f32>;
@group(0) @binding(1) var linearClamp: sampler;
@group(0) @binding(2) var<uniform> post: Post;
@group(0) @binding(3) var glow: texture_2d<f32>; // the composite's bloom

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

fn tap(uv: vec2f) -> vec3f {
  return textureSampleLevel(source, linearClamp, uv, 0.0).rgb;
}

// A 13-tap downsample (as in Call of Duty: Advanced Warfare's bloom): four
// overlapping 2x2 boxes and a centre one, weighted so it doesn't alias.
fn downsample(uv: vec2f) -> vec3f {
  let t = post.texel.xy;
  let a = tap(uv + t * vec2f(-2.0, -2.0));
  let b = tap(uv + t * vec2f(0.0, -2.0));
  let c = tap(uv + t * vec2f(2.0, -2.0));
  let d = tap(uv + t * vec2f(-2.0, 0.0));
  let e = tap(uv);
  let f = tap(uv + t * vec2f(2.0, 0.0));
  let g = tap(uv + t * vec2f(-2.0, 2.0));
  let h = tap(uv + t * vec2f(0.0, 2.0));
  let i = tap(uv + t * vec2f(2.0, 2.0));
  let j = tap(uv + t * vec2f(-1.0, -1.0));
  let k = tap(uv + t * vec2f(1.0, -1.0));
  let l = tap(uv + t * vec2f(-1.0, 1.0));
  let m = tap(uv + t * vec2f(1.0, 1.0));
  return e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
}

@fragment
fn fs_prefilter(in: Varyings) -> @location(0) vec4f {
  let color = downsample(in.uv);
  // Only light above the threshold, eased in: the part above it, x itself /
  // (itself + softness), so it starts gently and nothing at or below the
  // threshold blooms at all.
  let brightness = max(max(color.r, color.g), color.b);
  let excess = max(brightness - post.bloom.x, 0.0);
  let kept = excess * excess / (excess + post.bloom.y + 1e-5);
  return vec4f(color * (kept / max(brightness, 1e-5)), 1.0);
}

@fragment
fn fs_downsample(in: Varyings) -> @location(0) vec4f {
  return vec4f(downsample(in.uv), 1.0);
}

// Added (blended) onto the level above: a 3x3 tent spread by the radius.
@fragment
fn fs_upsample(in: Varyings) -> @location(0) vec4f {
  let t = post.texel.xy * post.bloom.z;
  var sum = tap(in.uv) * 4.0;
  sum += (tap(in.uv + vec2f(-t.x, 0.0)) + tap(in.uv + vec2f(t.x, 0.0)) + tap(in.uv + vec2f(0.0, -t.y)) +
          tap(in.uv + vec2f(0.0, t.y))) * 2.0;
  sum += tap(in.uv + vec2f(-t.x, -t.y)) + tap(in.uv + vec2f(t.x, -t.y)) + tap(in.uv + vec2f(-t.x, t.y)) +
         tap(in.uv + vec2f(t.x, t.y));
  return vec4f(sum / 16.0, 1.0);
}

@fragment
fn fs_composite(in: Varyings) -> @location(0) vec4f {
  let scene = textureLoad(source, vec2i(in.position.xy), 0).rgb;
  let bloom = textureSampleLevel(glow, linearClamp, in.uv, 0.0).rgb * post.bloom.w;
  // The target's alpha stays 1 (the editor shows the viewport as an image).
  return vec4f(min(scene + bloom, vec3f(1.0)), 1.0);
}
