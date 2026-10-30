// Shared by the 3D mesh shaders: first in each, then mesh3DSurface.wgsl, a
// material's surface (mesh3DStandardSurface.wgsl or a compiled material graph)
// and the pass's shader (mesh3DShadow / mesh3DLit). Must match
// WebGpuRenderer3D's FrameUniforms and vertex layouts.

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
