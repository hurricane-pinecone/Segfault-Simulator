// The renderer's passes after the lit scene: bloom, then the composite
// into the target. Each draws one full-screen triangle.
//
// Bloom: the prefilter keeps what of the light that blooms (the scene x each
// material's Bloom, which the lit pass writes beside it) is brighter than the
// threshold, while halving it; downsamples halve it again and again (a 13-tap
// filter, so the result doesn't shimmer); upsamples then add each level onto the one above
// it, spread by a tent filter, back up to the first. The composite adds that
// glow to the scene, then grades it (ColorGrading): exposure, white balance,
// contrast and saturation on the light, a tone mapper (or, with none, a clamp
// to 0..1), then lift / gamma / gain and a vignette. Ungraded (the default)
// it only clamps: lit surfaces never go above 1 then, so without bloom the
// picture is as it always was.

struct Post {
  texel: vec4f,    // xy the source's texel size
  bloom: vec4f,    // x threshold, y softness, z radius (texels), w intensity
  tone: vec4f,     // x tone mapper (0 none, 1 ACES, 2 AgX, 3 neutral), y exposure (x), z contrast, w saturation
  grade: vec4f,    // x graded (0: only clamp), y vignette, z its softness, w aspect (width / height)
  balance0: vec4f, // white balance: a 3x3 matrix's columns
  balance1: vec4f,
  balance2: vec4f,
  lift: vec4f,     // rgb
  gamma: vec4f,    // rgb 1 / gamma
  gain: vec4f,     // rgb
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

// --- Tone mappers: linear light in, linear 0..1 out ---------------------------

// ACES, as Stephen Hill fitted its RRT and ODT (sRGB in and out), with the
// usual 1 / 0.6 so mid-grey stays about where it was.
fn rrtAndOdtFit(v: vec3f) -> vec3f {
  let a = v * (v + 0.0245786) - 0.000090537;
  let b = v * (0.983729 * v + 0.4329510) + 0.238081;
  return a / b;
}

fn toneAces(color: vec3f) -> vec3f {
  // Columns of Hill's matrices (which he gives as rows).
  let toAces = mat3x3f(vec3f(0.59719, 0.07600, 0.02840),
                       vec3f(0.35458, 0.90834, 0.13383),
                       vec3f(0.04823, 0.01566, 0.83777));
  let fromAces = mat3x3f(vec3f(1.60475, -0.10208, -0.00327),
                         vec3f(-0.53108, 1.10813, -0.07276),
                         vec3f(-0.07367, -0.00605, 1.07602));
  return clamp(fromAces * rrtAndOdtFit(toAces * (color / 0.6)), vec3f(0.0), vec3f(1.0));
}

// AgX (Troy Sobotka's; this is Benjamin Wrensch's fit of its default look):
// into AgX's log space, its S-curve, and back out through the display's 2.2.
fn agxCurve(x: vec3f) -> vec3f {
  let x2 = x * x;
  let x4 = x2 * x2;
  return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

fn toneAgx(color: vec3f) -> vec3f {
  let inset = mat3x3f(vec3f(0.842479062253094, 0.0423282422610123, 0.0423756549057051),
                      vec3f(0.0784335999999992, 0.878468636469772, 0.0784336),
                      vec3f(0.0792237451477643, 0.0791661274605434, 0.879142973793104));
  let outset = mat3x3f(vec3f(1.19687900512017, -0.0528968517574562, -0.0529716355144438),
                       vec3f(-0.0980208811401368, 1.15190312990417, -0.0980434501171241),
                       vec3f(-0.0990297440797205, -0.0989611768448433, 1.15107367264116));
  let minEv = -12.47393;
  let maxEv = 4.026069;
  var v = inset * max(color, vec3f(1e-10));
  v = clamp(log2(v), vec3f(minEv), vec3f(maxEv));
  v = agxCurve((v - minEv) / (maxEv - minEv));
  v = outset * v;
  return pow(clamp(v, vec3f(0.0), vec3f(1.0)), vec3f(2.2));
}

// Khronos PBR Neutral: the materials' colours kept, only the brightest
// compressed (and desaturated as they are).
fn toneNeutral(color: vec3f) -> vec3f {
  let startCompression = 0.8 - 0.04;
  let desaturation = 0.15;
  let x = min(color.r, min(color.g, color.b));
  let offset = select(0.04, x - 6.25 * x * x, x < 0.08);
  var c = color - offset;
  let peak = max(c.r, max(c.g, c.b));
  if (peak < startCompression) {
    return clamp(c, vec3f(0.0), vec3f(1.0));
  }
  let d = 1.0 - startCompression;
  let newPeak = 1.0 - d * d / (peak + d - startCompression);
  c *= newPeak / peak;
  let g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
  return clamp(mix(c, vec3f(newPeak), g), vec3f(0.0), vec3f(1.0));
}

fn luminance(c: vec3f) -> f32 {
  return dot(c, vec3f(0.2126, 0.7152, 0.0722));
}

// The light graded into what the screen shows (ColorGrading's order).
fn grade(light: vec3f, uv: vec2f) -> vec3f {
  // On the light: exposure, white balance, contrast (around mid-grey, in
  // stops) and saturation.
  var c = max(light * post.tone.y, vec3f(0.0));
  c = max(mat3x3f(post.balance0.xyz, post.balance1.xyz, post.balance2.xyz) * c, vec3f(0.0));
  let midGrey = 0.18;
  c = midGrey * pow(max(c, vec3f(1e-6)) / midGrey, vec3f(post.tone.z));
  c = max(mix(vec3f(luminance(c)), c, post.tone.w), vec3f(0.0));

  // Into the screen's 0..1.
  let mapper = i32(post.tone.x + 0.5);
  if (mapper == 1) {
    c = toneAces(c);
  } else if (mapper == 2) {
    c = toneAgx(c);
  } else if (mapper == 3) {
    c = toneNeutral(c);
  } else {
    c = min(c, vec3f(1.0));
  }

  // On what the screen shows: lift, gain, gamma; then the vignette (from the
  // middle, 0, to the corners, 1, round on any screen).
  c = post.gain.rgb * (c + post.lift.rgb * (vec3f(1.0) - c));
  c = pow(clamp(c, vec3f(0.0), vec3f(1.0)), post.gamma.rgb);
  let fromMiddle = (uv - 0.5) * vec2f(post.grade.w, 1.0);
  let reach = length(fromMiddle) / length(vec2f(post.grade.w, 1.0) * 0.5);
  let edge = smoothstep(mix(0.9, 0.15, post.grade.z), 1.05, reach);
  return c * (1.0 - post.grade.y * edge);
}

@fragment
fn fs_composite(in: Varyings) -> @location(0) vec4f {
  let scene = textureLoad(source, vec2i(in.position.xy), 0).rgb;
  let bloom = textureSampleLevel(glow, linearClamp, in.uv, 0.0).rgb * post.bloom.w;
  // The target's alpha stays 1 (the editor shows the viewport as an image).
  if (post.grade.x < 0.5) {
    return vec4f(min(scene + bloom, vec3f(1.0)), 1.0);
  }
  return vec4f(grade(scene + bloom, in.uv), 1.0);
}
