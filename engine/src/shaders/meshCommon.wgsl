// Shared by the 3D mesh shaders: first in each, then meshSurface.wgsl, a
// material's surface (meshStandardSurface.wgsl or a compiled material graph)
// and the pass's shader (meshShadow / meshLit). Must match
// WebGpuRenderer's FrameUniforms and vertex layouts.

struct Frame {
  viewProjection: mat4x4f,
  lightViewProjection: mat4x4f,
  cameraPosition: vec4f,
  sunDirection: vec4f,  // xyz toward the sun (unit), w diffuse strength
  sunColor: vec4f,      // rgb sunlight tint, w ambient level
  shadow: vec4f,        // x enabled (0/1), y depth bias, z filter spread (uv), w world size of a texel
  sky: vec4f,           // rgb sky colour (what shiny surfaces reflect)
  time: vec4f,          // x seconds the scene has run
  cameraForward: vec4f, // xyz unit view direction
  clusterSize: vec4u,   // the light clusters: x tiles across, y down, z depth slices, w lights
  clusterDepth: vec4f,  // x near, y far, z slices per log(depth / near), w tile size (pixels)
  wind: vec4f,          // xy which way it blows (unit), z strength, w gustiness
  haze: vec4f,          // x strength, y 1 / the distance it's half that
  sunBalance: vec4f,    // rgb the sun's share of a surface's light x this, w 1 if re-balanced (a visual style)
  ambientBalance: vec4f, // rgb ambient's share x this, w the brightest light gets (1 unless tone mapped)
  inverseViewProjection: mat4x4f, // clip space back to the world (the background's view rays)
  // The sun's shadow cascades (cascadeInfo.x > 1; else lightViewProjection's one map).
  cascades: array<mat4x4f, 4>,
  cascadeRects: array<vec4f, 4>,  // where each is in the shadow atlas (uv): xy corner, zw size
  cascadeParams: array<vec4f, 4>, // x world size of a texel, y depth bias, zw filter spread (atlas uv)
  cascadeInfo: vec4f,  // x how many, y the blend band (of a cascade's half size), z where shadows
                       // start fading out, w where they're gone (along the view)
  skyMode: vec4f,      // x physical (0/1), y its reflections ready (0/1), z their roughest mip,
                       // w the night glow's strength (stars)
  sunLight: vec4f,     // rgb a physical sky's sun on a surface facing it, w its disk's radius (radians)
  skyScale: vec4f,     // rgb a visual style's scale on the sky, w the sun disk's brightness
  skyIrradiance: array<vec4f, 9>, // a physical sky's diffuse light: SH9 (rgb), convolved, / pi
  skyLight: vec4f,     // x what eyes adapted to a dim sky scale its light by (its reflections: the
                       // rest is already scaled), y how much of the sun's light clears the horizon
                       // (sunLight is faded by it; its disk isn't), z what they scale lamps by
                       // (point lights, emission: lit for eyes adapted to the night)
  ambientOcclusion: vec4f, // x on (0/1: its map is read), y how near in depth (of a pixel's) the
                           // map's must be to count
  fog: vec4f,          // height fog: x density, y height falloff, z base height, w start distance
  fogLight: vec4f,     // x the most it hides, y its glow toward the sun, z on (0/1), w how far
                       // away the background is
  moon: vec4f,         // a physical sky's moon: xyz toward it, w its disk's radius (0: none drawn)
  moonSun: vec4f,      // xyz toward the sun (the moon's lit side), w how much of it is lit (0..1)
  moonLight: vec4f,    // rgb its disk's light (not adapted), w 1 if it's the frame's light (in the
                       // sun's place: sunDirection, sunLight)
}

@group(0) @binding(0) var<uniform> frame: Frame;

// Per-vertex mesh data (buffer 0).
struct VertexIn {
  @location(0) position: vec3f,
  @location(1) normal: vec3f,
  @location(2) color: vec3f,
  @location(8) uv: vec2f,  // after the instance locations (3-7)
}

// Per-instance data (buffer 1): model matrix columns, tint, and the rectangle
// of its textures it shows (xy corner, zw size; a sprite sheet's frame).
struct InstanceIn {
  @location(3) model0: vec4f,
  @location(4) model1: vec4f,
  @location(5) model2: vec4f,
  @location(6) model3: vec4f,
  @location(7) tint: vec4f,
  @location(9) uvRect: vec4f,
}

fn instanceModel(instance: InstanceIn) -> mat4x4f {
  return mat4x4f(instance.model0, instance.model1, instance.model2, instance.model3);
}

// The vertex's UV within its instance's rectangle.
fn instanceUv(vertex: VertexIn, instance: InstanceIn) -> vec2f {
  return instance.uvRect.xy + vertex.uv * instance.uvRect.zw;
}

// Turns a mesh's normals into world space: the inverse-transpose of the model
// matrix's upper 3x3, so non-uniform scales keep normals perpendicular to
// their surfaces.
fn normalMatrix(model: mat4x4f) -> mat3x3f {
  let m = mat3x3f(model[0].xyz, model[1].xyz, model[2].xyz);
  let c0 = cross(m[1], m[2]);
  let c1 = cross(m[2], m[0]);
  let c2 = cross(m[0], m[1]);
  let det = dot(m[0], c0);
  return mat3x3f(c0, c1, c2) * (1.0 / det);
}
