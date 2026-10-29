// The UI (UiModule), over the finished frame: one instance per UiQuad, two
// triangles each. Boxes are rounded rectangles drawn from their distance
// field (smooth corners, soft shadows, a border ring); glyphs and images come
// from the batch's texture.

struct Ui {
  screen: vec4f, // xy the target's size in pixels, z 1 if it's sRGB (so colours are converted)
}

@group(0) @binding(0) var<uniform> ui: Ui;
@group(0) @binding(1) var uiSampler: sampler;
@group(1) @binding(0) var uiTexture: texture_2d<f32>;

struct QuadIn {
  @location(0) rect: vec4f,        // x, y, width, height (pixels)
  @location(1) uv: vec4f,          // u, v, width, height
  @location(2) color: vec4f,       // sRGB, alpha
  @location(3) borderColor: vec4f, // sRGB, alpha
  @location(4) shape: vec4f,       // radius, softness, border width, kind (0 box, 1 glyph, 2 image)
}

struct Varyings {
  @builtin(position) position: vec4f,
  @location(0) local: vec2f, // from the rect's middle, pixels
  @location(1) uv: vec2f,
  @location(2) color: vec4f,
  @location(3) borderColor: vec4f,
  @location(4) @interpolate(flat) shape: vec4f,
  @location(5) @interpolate(flat) half: vec2f,
}

@vertex
fn vs_main(@builtin(vertex_index) index: u32, quad: QuadIn) -> Varyings {
  let corners = array<vec2f, 6>(vec2f(0.0, 0.0), vec2f(1.0, 0.0), vec2f(0.0, 1.0),
                                vec2f(0.0, 1.0), vec2f(1.0, 0.0), vec2f(1.0, 1.0));
  let corner = corners[index];
  // Boxes grow by their softness (and a pixel), so their fading edge fits.
  let pad = select(0.0, quad.shape.y + 1.0, quad.shape.w < 0.5);
  let point = quad.rect.xy - pad + corner * (quad.rect.zw + 2.0 * pad);
  var out: Varyings;
  out.position = vec4f(point / ui.screen.xy * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
  out.local = point - (quad.rect.xy + quad.rect.zw * 0.5);
  out.uv = quad.uv.xy + (point - quad.rect.xy) / max(quad.rect.zw, vec2f(1e-4)) * quad.uv.zw;
  out.color = quad.color;
  out.borderColor = quad.borderColor;
  out.shape = quad.shape;
  out.half = quad.rect.zw * 0.5;
  return out;
}

fn toLinear(c: vec3f) -> vec3f {
  return select(pow((c + 0.055) / 1.055, vec3f(2.4)), c / 12.92, c <= vec3f(0.04045));
}

@fragment
fn fs_main(in: Varyings) -> @location(0) vec4f {
  let texel = textureSample(uiTexture, uiSampler, in.uv);
  var color = in.color;
  let kind = in.shape.w;
  if (kind > 1.5) {
    color = texel * in.color; // an image
  } else if (kind > 0.5) {
    // A glyph. Blending in linear light thins dark text on light ground:
    // dark text's coverage is strengthened to match how it looks light.
    let lightness = dot(in.color.rgb, vec3f(0.299, 0.587, 0.114));
    let coverage = pow(texel.a, mix(0.6, 1.0, lightness));
    color = vec4f(in.color.rgb, in.color.a * coverage);
  } else {
    // A rounded box: its distance field, faded over its softness (at least a
    // pixel, so its edge is smooth), a border ring inside its edge.
    let radius = min(in.shape.x, min(in.half.x, in.half.y));
    let q = abs(in.local) - in.half + radius;
    let distance = length(max(q, vec2f(0.0))) + min(max(q.x, q.y), 0.0) - radius;
    let soft = max(in.shape.y, 1.0);
    let coverage = 1.0 - smoothstep(-soft * 0.5, soft * 0.5, distance);
    if (in.shape.z > 0.0) {
      // The ring over the fill.
      let ring = smoothstep(-0.5, 0.5, distance + in.shape.z) * in.borderColor.a;
      color = vec4f(mix(in.color.rgb, in.borderColor.rgb, ring), in.color.a + (1.0 - in.color.a) * ring);
    }
    color.a *= coverage;
  }
  if (ui.screen.z > 0.5) {
    color = vec4f(toLinear(color.rgb), color.a);
  }
  return color;
}
