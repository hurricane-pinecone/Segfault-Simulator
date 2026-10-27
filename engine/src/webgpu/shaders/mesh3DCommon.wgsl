// Shared by the 3D mesh shaders (prefixed to mesh3DShadow / mesh3DLit). Must
// match WebGpuRenderer3D's FrameUniforms and vertex layouts.

const MAX_POINT_LIGHTS: u32 = 32u;

struct PointLight {
  positionRadius: vec4f,  // xyz world position, w reach
  colorIntensity: vec4f,  // rgb tint, w intensity
}

struct Frame {
  viewProjection: mat4x4f,
  lightViewProjection: mat4x4f,
  cameraPosition: vec4f,
  sunDirection: vec4f,  // xyz toward the sun (unit), w diffuse strength
  sunColor: vec4f,      // rgb sunlight tint, w ambient level
  shadow: vec4f,        // x enabled (0/1), y depth bias, z filter spread (uv), w world size of a texel
  lightCount: vec4u,    // x active point lights
  lights: array<PointLight, MAX_POINT_LIGHTS>,
}

@group(0) @binding(0) var<uniform> frame: Frame;

// Per-vertex mesh data (buffer 0).
struct VertexIn {
  @location(0) position: vec3f,
  @location(1) normal: vec3f,
  @location(2) color: vec3f,
}

// Per-instance data (buffer 1): model matrix columns + tint.
struct InstanceIn {
  @location(3) model0: vec4f,
  @location(4) model1: vec4f,
  @location(5) model2: vec4f,
  @location(6) model3: vec4f,
  @location(7) tint: vec4f,
}

fn instanceModel(instance: InstanceIn) -> mat4x4f {
  return mat4x4f(instance.model0, instance.model1, instance.model2, instance.model3);
}
