// The standard surface (MeshMaterial without a shader): glTF's
// metallic-roughness material. The albedo is the vertex colour x baseColor x
// the base texture; alpha is baseColor's x the texture's; the normal map
// (flat without one) tilts the normal, deeper by its scale. Must match
// WebGpuRenderer3D's MaterialUniforms.

struct Material {
  baseColor: vec4f,   // linear rgb, and alpha
  params: vec4f,      // x normal scale, y metallic, z roughness, w alpha cutoff
  uvTransform: vec4f, // xy tiling, zw offset
  mode: vec4u,        // x alpha mode, y unlit (its colour shown as it is)
}

@group(1) @binding(0) var baseTexture: texture_2d<f32>;      // sRGB: samples come back linear
@group(1) @binding(1) var materialSampler: sampler;
@group(1) @binding(2) var metallicRoughnessTexture: texture_2d<f32>; // b metal, g roughness
@group(1) @binding(3) var<uniform> material: Material;
@group(1) @binding(4) var normalTexture: texture_2d<f32>; // tangent space, read as data

fn surface(in: SurfaceIn) -> Surface {
  // Where the texture coordinates land in the material's textures.
  let uv = in.uv * material.uvTransform.xy + material.uvTransform.zw;
  let base = textureSample(baseTexture, materialSampler, uv);
  let metallicRoughness = textureSample(metallicRoughnessTexture, materialSampler, uv);
  let bump = textureSample(normalTexture, materialSampler, uv).xyz * 2.0 - 1.0;

  var result: Surface;
  result.baseColor = in.vertexColor * material.baseColor.rgb * base.rgb;
  result.alpha = base.a * material.baseColor.a;
  result.metallic = material.params.y * metallicRoughness.b;
  result.roughness = material.params.z * metallicRoughness.g;
  result.normal = normalize(vec3f(bump.xy * material.params.x, max(bump.z, 0.0001)));
  result.emission = vec3f(0.0);
  if (material.mode.y != 0u) {
    result.emission = result.baseColor;
    result.baseColor = vec3f(0.0);
  }
  result.alphaClip = material.params.w;
  result.bloom = 1.0;
  return result;
}

fn surfaceAlphaMode() -> u32 {
  return material.mode.x;
}
