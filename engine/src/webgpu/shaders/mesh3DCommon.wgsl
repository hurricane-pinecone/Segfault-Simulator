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
  sky: vec4f,           // rgb sky colour (what shiny surfaces reflect)
  lightCount: vec4u,    // x active point lights
  lights: array<PointLight, MAX_POINT_LIGHTS>,
}

@group(0) @binding(0) var<uniform> frame: Frame;

// Per-vertex mesh data (buffer 0).
struct VertexIn {
  @location(0) position: vec3f,
  @location(1) normal: vec3f,
  @location(2) color: vec3f,
  @location(8) uv: vec2f,  // after the instance locations (3-7)
}

// Per-instance data (buffer 1): model matrix columns + tint.
struct InstanceIn {
  @location(3) model0: vec4f,
  @location(4) model1: vec4f,
  @location(5) model2: vec4f,
  @location(6) model3: vec4f,
  @location(7) tint: vec4f,
}

// The mesh section's material (group 1); must match WebGpuRenderer3D's
// MaterialUniforms and AlphaMode.
const ALPHA_OPAQUE: u32 = 0u;
const ALPHA_MASK: u32 = 1u;
const ALPHA_BLEND: u32 = 2u;

struct Material {
  baseColor: vec4f, // linear rgb, and alpha
  params: vec4f,    // y metallic, z roughness, w alpha cutoff
  uvTransform: vec4f, // xy tiling, zw offset
  mode: vec4u,      // x alpha mode
}

// Where a vertex's texture coordinates land in the material's textures.
fn materialUv(uv: vec2f) -> vec2f {
  return uv * material.uvTransform.xy + material.uvTransform.zw;
}

@group(1) @binding(0) var baseTexture: texture_2d<f32>;      // sRGB: samples come back linear
@group(1) @binding(1) var materialSampler: sampler;
@group(1) @binding(2) var metallicRoughnessTexture: texture_2d<f32>; // b metal, g roughness
@group(1) @binding(3) var<uniform> material: Material;

// A fragment's opacity: the texture's and material's alpha (unless the
// material is opaque) times the instance tint's.
fn surfaceAlpha(textureAlpha: f32, tintAlpha: f32) -> f32 {
  if (material.mode.x == ALPHA_OPAQUE) {
    return tintAlpha;
  }
  return textureAlpha * material.baseColor.a * tintAlpha;
}

// Below this a fragment is cut out: the mask cutoff, or half for see-through
// surfaces (so only their solid parts cast shadows).
fn alphaCutoff() -> f32 {
  return select(0.5, material.params.w, material.mode.x == ALPHA_MASK);
}

fn instanceModel(instance: InstanceIn) -> mat4x4f {
  return mat4x4f(instance.model0, instance.model1, instance.model2, instance.model3);
}
