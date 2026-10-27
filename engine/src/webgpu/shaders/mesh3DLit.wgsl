// Lit forward pass for 3D meshes. (mesh3DCommon.wgsl is prefixed.)
//
// The lighting model is the isometric renderer's (geometry.frag) carried into
// real 3D, so both paths share a look: ambient that favours up-facing
// surfaces, a sun-diffuse term that sides turned away from the sun drop to a
// "shaded" level, cast shadows that pull a surface to that shaded level, and
// point lights that fade in as daylight goes. Only the shadow source differs:
// a sun shadow map instead of a heightmap march.

@group(0) @binding(1) var shadowMap: texture_depth_2d;
@group(0) @binding(2) var shadowSampler: sampler_comparison;

struct VertexOut {
  @builtin(position) clip: vec4f,
  @location(0) worldPosition: vec3f,
  @location(1) normal: vec3f,
  @location(2) albedo: vec3f,
}

@vertex
fn vs_main(vertex: VertexIn, instance: InstanceIn) -> VertexOut {
  let model = instanceModel(instance);
  let world = model * vec4f(vertex.position, 1.0);

  // Inverse-transpose of the upper 3x3, so non-uniform scales keep normals
  // perpendicular to their surfaces.
  let linear = mat3x3f(model[0].xyz, model[1].xyz, model[2].xyz);
  let normalMatrix = transpose(inverse3x3(linear));

  var out: VertexOut;
  out.clip = frame.viewProjection * world;
  out.worldPosition = world.xyz;
  out.normal = normalMatrix * vertex.normal;
  out.albedo = vertex.color * instance.tint.rgb;
  return out;
}

fn inverse3x3(m: mat3x3f) -> mat3x3f {
  let c0 = cross(m[1], m[2]);
  let c1 = cross(m[2], m[0]);
  let c2 = cross(m[0], m[1]);
  let det = dot(m[0], c0);
  return transpose(mat3x3f(c0, c1, c2)) * (1.0 / det);
}

// Sun visibility: 1 = lit, 0 = fully in shadow. A grid of hardware-filtered
// comparison taps, spread by `softness` texels, gives soft edges.
fn sunVisibility(worldPosition: vec3f, normal: vec3f) -> f32 {
  if (frame.shadow.x < 0.5) {
    return 1.0;
  }

  // Nudge the lookup off the surface along its normal, by a texel and a half
  // of world distance, so a surface never shadows itself at grazing sun
  // angles.
  let offsetPosition = worldPosition + normal * (frame.shadow.w * 1.5);
  let lightClip = frame.lightViewProjection * vec4f(offsetPosition, 1.0);
  let ndc = lightClip.xyz / lightClip.w;
  let uv = vec2f(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);

  // Outside the shadow map's coverage: treat as lit.
  if (any(uv < vec2f(0.0)) || any(uv > vec2f(1.0)) || ndc.z > 1.0) {
    return 1.0;
  }

  let depth = ndc.z - frame.shadow.y;
  let spread = frame.shadow.z;
  var sum = 0.0;
  for (var y = -1; y <= 1; y++) {
    for (var x = -1; x <= 1; x++) {
      let tap = uv + vec2f(f32(x), f32(y)) * spread;
      sum += textureSampleCompareLevel(shadowMap, shadowSampler, tap, depth);
    }
  }
  return sum / 9.0;
}

// Point-light contribution, as in the isometric renderer: each light adds a
// smootherstep-attenuated, softened diffuse term; colours blend by weight and
// the total brightness is capped so overlapping lamps don't blow out.
fn pointLighting(worldPosition: vec3f, normal: vec3f) -> vec3f {
  var weightedColor = vec3f(0.0);
  var totalWeight = 0.0;
  var strongest = 0.0;

  for (var i = 0u; i < frame.lightCount.x; i++) {
    let light = frame.lights[i];
    let toLight = light.positionRadius.xyz - worldPosition;
    let dist = length(toLight);
    let radius = light.positionRadius.w;
    if (dist >= radius) {
      continue;
    }

    let reach = clamp(1.0 - dist / radius, 0.0, 1.0);
    let attenuation = reach * reach * reach * (reach * (reach * 6.0 - 15.0) + 10.0);
    let ndotl = max(dot(normal, toLight / max(dist, 1e-4)), 0.0);
    let diffuse = mix(0.12, 1.0, pow(ndotl, 0.8));
    let amount = light.colorIntensity.w * attenuation * diffuse;

    var color = light.colorIntensity.rgb;
    let maxChannel = max(max(color.r, color.g), color.b);
    color = select(vec3f(1.0), color / maxChannel, maxChannel > 0.001);

    weightedColor += color * amount;
    totalWeight += amount;
    strongest = max(strongest, amount);
  }

  if (totalWeight <= 0.001) {
    return vec3f(0.0);
  }

  let capped = (strongest / (1.0 + strongest)) * 1.65;
  return (weightedColor / totalWeight) * capped;
}

@fragment
fn fs_main(in: VertexOut) -> @location(0) vec4f {
  let normal = normalize(in.normal);
  let toSun = frame.sunDirection.xyz;
  let diffuseStrength = frame.sunDirection.w;
  let ambient = frame.sunColor.w;

  let sunDiffuse = max(dot(normal, toSun), 0.0);

  // Up-facing surfaces take full ambient; sides turned away from the sun fall
  // to the shaded level.
  let upness = smoothstep(0.0, 0.5, normal.z);
  let sunFacing = smoothstep(0.0, 0.6, sunDiffuse);
  let shade = (1.0 - upness) * (1.0 - sunFacing);

  let sunHeight = clamp(toSun.z, 0.0, 1.0);
  let ambientLevel = ambient * mix(mix(0.8, 1.0, sunHeight), 1.0, 1.0 - upness);

  let litLevel = ambientLevel + sunDiffuse * diffuseStrength;
  let shadedLevel = min(1.0 - diffuseStrength, ambient);
  var lit = mix(litLevel, shadedLevel, shade);

  // Cast shadows keep ambient but lose the direct sun.
  lit = mix(shadedLevel, lit, sunVisibility(in.worldPosition, normal));

  let sunlight = max(vec3f(lit) * frame.sunColor.rgb, vec3f(0.03));

  // Point lights fade in as daylight goes, and only brighten what sunlight
  // hasn't already saturated.
  let daylight = smoothstep(0.20, 0.75, ambient);
  var pointVisibility = 1.0 - daylight;
  pointVisibility *= pointVisibility;

  var point = vec3f(0.0);
  if (pointVisibility > 0.001) {
    point = pointLighting(in.worldPosition, normal) * (pointVisibility * 2.0);
  }

  let sunlightAmount = max(max(sunlight.r, sunlight.g), sunlight.b);
  let shadowRoom = mix(1.0, 1.0 - smoothstep(0.65, 1.0, sunlightAmount), daylight);

  let total = clamp(sunlight + point * shadowRoom, vec3f(0.0), vec3f(1.0));
  return vec4f(in.albedo * total, 1.0);
}
