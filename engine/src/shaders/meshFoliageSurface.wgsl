// The foliage surface (MeshMaterial::foliage): a tree's or a plant's bark and
// leaf cards. The standard surface's textures and parameters, with the mesh's
// vertex colours read as data (R how open the point is, x the albedo; G how
// much it sways with its branch; B how much it flutters), its vertices moved
// in the frame's wind, sunlight let through from behind (x the
// metallic-roughness texture's red: the leaves' thinness), its normals kept on
// back faces, its cut-outs kept solid as the texture shrinks into mipmaps,
// its instance's dithered fade (level-of-detail cross-fades), and fading out of
// the camera's way (MeshMaterial::fadeNearCamera). Always cut
// out (ALPHA_MASK), so even its bark can fade. Must match WebGpuRenderer's
// FoliageUniforms.

struct Material {
  baseColor: vec4f,   // linear rgb, and alpha
  params: vec4f,      // x normal scale, y metallic, z roughness, w alpha cutoff
  uvTransform: vec4f, // xy tiling, zw offset
  mode: vec4u,        // x alpha mode, y unlit, z fadeNearCamera (f32's bits)
  foliage: vec4f,     // x translucency, y bend, z sway, w flutter
}

@group(1) @binding(0) var baseTexture: texture_2d<f32>;      // sRGB: samples come back linear
@group(1) @binding(1) var materialSampler: sampler;
@group(1) @binding(2) var metallicRoughnessTexture: texture_2d<f32>; // r thinness, g roughness, b metal
@group(1) @binding(3) var<uniform> material: Material;
@group(1) @binding(4) var normalTexture: texture_2d<f32>; // tangent space, read as data

// A number from 0 to 1 for a point (the same every frame).
fn foliageHash(p: vec2f) -> f32 {
  return fract(sin(dot(p, vec2f(12.9898, 78.233))) * 43758.5453);
}

// Moved in the wind: gusts rolling across the land the way it blows (as
// grass's do), so neighbouring trees move together; each tree bending over
// by its height above where it stands (more the taller), its branches
// swaying on their own (G) and its leaves fluttering (B), each a little out
// of step with the next.
fn surfaceVertex(v: SurfaceVertex) -> SurfaceVertex {
  var out = v;
  let wind = frame.wind;
  if (wind.z <= 0.0) {
    return out;
  }
  let time = frame.time.x;
  let seed = foliageHash(v.origin.xy);
  let wave = sin(dot(v.origin.xy, wind.xy) * 0.35 - time * 1.7) * 0.5 + 0.5;
  let gust = mix(0.75, 0.3 + wave * 1.1, wind.w);
  let across = vec2f(-wind.y, wind.x);

  // The whole tree: downwind with the gusts, swinging a little about that.
  let height = max(v.position.z - v.origin.z, 0.0);
  let swing = sin(time * (0.7 + 0.3 * seed) + seed * 6.2831853);
  let lean = material.foliage.y * wind.z * 0.0025 * height * height;
  var offset = vec3f(wind.xy * (lean * (gust + 0.2 * swing)) + across * (lean * 0.12 * swing), 0.0);

  // Its branches, each part a little out of step with the next.
  let branch = v.color.g * material.foliage.z * wind.z;
  if (branch > 0.0) {
    let phase = dot(v.position, vec3f(0.31, 0.27, 0.19)) + seed * 6.2831853;
    let sway = sin(time * 1.3 + phase) * 0.6 + sin(time * 2.3 + phase * 1.7) * 0.4;
    offset += vec3f(wind.xy * (0.35 * gust + 0.25 * sway) + across * (0.2 * sway), 0.12 * sway) * (branch * 0.5);
  }

  // Its leaves, quickly and only a little, across their cards.
  let leaf = v.color.b * material.foliage.w * wind.z;
  if (leaf > 0.0) {
    let phase = dot(v.position, vec3f(2.1, 1.7, 2.9)) + seed * 17.0;
    offset += v.normal * (leaf * 0.06 * (0.5 + gust) * sin(time * 8.0 + phase));
  }
  out.position = v.position + offset;
  return out;
}

fn surfaceFlipsBackFaces() -> bool {
  return false; // a crown's normals are bent out from its middle, either side
}

// How solid a point is: the texture's alpha, kept up as it shrinks into
// mipmaps (where it averages leaves with the gaps between them, and would cut
// a far crown away to nothing), and nothing where its instance has faded.
fn foliageAlpha(in: SurfaceIn, uv: vec2f, alpha: f32) -> f32 {
  let size = vec2f(textureDimensions(baseTexture));
  let texels = max(length(dpdx(uv * size)), length(dpdy(uv * size)));
  let mip = max(log2(max(texels, 1e-6)), 0.0);
  var kept = alpha * (1.0 + mip * 0.25);
  if (ditherFaded(in.fade, in.pixel) || cameraFaded(in, bitcast<f32>(material.mode.z))) {
    kept = 0.0;
  }
  return kept;
}

fn surface(in: SurfaceIn) -> Surface {
  let uv = in.uv * material.uvTransform.xy + material.uvTransform.zw;
  let base = textureSample(baseTexture, materialSampler, uv);
  let thinRough = textureSample(metallicRoughnessTexture, materialSampler, uv);
  let bump = textureSample(normalTexture, materialSampler, uv).xyz * 2.0 - 1.0;

  var result: Surface;
  // (Never quite black inside a crown: light gets in between the leaves.)
  result.baseColor = material.baseColor.rgb * base.rgb * (0.3 + 0.7 * clamp(in.vertexColor.r, 0.0, 1.0));
  result.alpha = foliageAlpha(in, uv, base.a * material.baseColor.a);
  result.metallic = material.params.y * thinRough.b;
  result.roughness = material.params.z * thinRough.g;
  result.normal = normalize(vec3f(bump.xy * material.params.x, max(bump.z, 0.0001)));
  result.emission = vec3f(0.0);
  result.alphaClip = material.params.w;
  result.bloom = 1.0;
  result.translucency = material.foliage.x * thinRough.r;
  return result;
}

fn surfaceAlpha(in: SurfaceIn) -> vec2f {
  let uv = in.uv * material.uvTransform.xy + material.uvTransform.zw;
  let base = textureSample(baseTexture, materialSampler, uv);
  return vec2f(foliageAlpha(in, uv, base.a * material.baseColor.a), material.params.w);
}

fn surfaceAlphaMode() -> u32 {
  return ALPHA_MASK;
}
