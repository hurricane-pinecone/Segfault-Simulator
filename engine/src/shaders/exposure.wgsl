// Auto exposure (ColorGrading::autoExposure), before the composite: one pixel,
// drawn each frame, holding the exposure correction in stops (r). It meters the
// lit scene on a grid of points (the middle counting more), as the average of
// their brightness in stops, works out the correction that brings that to the
// target (within the limits), and eases the last frame's (`previous`, a copy
// of this pixel) toward it, as eyes adjust. Off, it's 0: no correction.

struct Exposure {
  params: vec4f, // x the target (log2 brightness), y the most it darkens, z brightens (stops),
                 // w how far toward what it wants it eases this frame (0..1)
  source: vec4f, // xy the scene's size (texels), z on (0/1)
}

@group(0) @binding(0) var scene: texture_2d<f32>;
@group(0) @binding(1) var previous: texture_2d<f32>;
@group(0) @binding(2) var<uniform> exposure: Exposure;

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4f {
  // One triangle covering the (one-pixel) target.
  let corner = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
  return vec4f(corner * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
}

const kAcross: i32 = 32;
const kDown: i32 = 18;

@fragment
fn fs_meter() -> @location(0) vec4f {
  if (exposure.source.z < 0.5) {
    return vec4f(0.0, 0.0, 0.0, 1.0);
  }
  let size = exposure.source.xy;
  var total = 0.0;
  var weight = 0.0;
  for (var y = 0; y < kDown; y++) {
    for (var x = 0; x < kAcross; x++) {
      let uv = (vec2f(f32(x), f32(y)) + 0.5) / vec2f(f32(kAcross), f32(kDown));
      let texel = vec2i(min(uv * size, size - 1.0));
      let light = dot(textureLoad(scene, texel, 0).rgb, vec3f(0.2126, 0.7152, 0.0722));
      let w = 1.0 - 0.6 * length((uv - 0.5) * 2.0) / sqrt(2.0);
      total += log2(max(light, 1e-4)) * w;
      weight += w;
    }
  }
  let wanted = clamp(exposure.params.x - total / weight, -exposure.params.y, exposure.params.z);
  let before = textureLoad(previous, vec2i(0), 0).r;
  return vec4f(mix(before, wanted, exposure.params.w), 0.0, 0.0, 1.0);
}
