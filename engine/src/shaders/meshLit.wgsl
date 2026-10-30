// Lit forward pass for 3D meshes: its lighting and fragment stage. (meshCommon,
// meshSurface and the material's surface are prefixed; a vertex stage
// writing VertexOut follows: meshLitVertex.wgsl for meshes,
// meshGrass.wgsl for grass.)
//
// The lighting model is the isometric renderer's (geometry.frag) carried into
// real 3D, so both paths share a look: ambient that favours up-facing
// surfaces, a sun-diffuse term that sides turned away from the sun drop to a
// "shaded" level, cast shadows that pull a surface to that shaded level, and
// point lights that brighten what sunlight hasn't (shade by day, everything at
// night). Only the shadow source differs: a sun shadow map instead of a
// heightmap march.
//
// It writes the scene's HDR colour: lit surfaces stay within 0..1, but
// emission and bright glints can go above, which is what blooms (see
// post.wgsl, whose composite clamps to the target). And beside it, the light
// that blooms: the colour x the material's Bloom, so a material can bloom more
// or less than its brightness alone would.

@group(0) @binding(1) var shadowMap: texture_depth_2d;
@group(0) @binding(2) var shadowSampler: sampler_comparison;

const PI: f32 = 3.14159265;

struct VertexOut {
  @builtin(position) clip: vec4f,
  @location(0) worldPosition: vec3f,
  @location(1) normal: vec3f,
  @location(2) vertexColor: vec3f,
  @location(3) uv: vec2f,
  @location(4) tint: vec4f,
  @location(5) grass: vec4f,     // see SurfaceIn (others: 1, 0, 0, 0)
  @location(6) grassData: vec4f, // see SurfaceIn
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

// The frame's point lights, and which of them each cluster (a screen tile x a
// depth slice) lists; see lightClusters.h, whose ClusterGrid this must match.
struct PointLight {
  positionRadius: vec4f, // xyz world position, w reach
  colorIntensity: vec4f, // rgb tint, w intensity
  shadow: vec4f,         // x its shadow map's layer (-1: none), y 1 / its face size,
                         // z how big it is (its source's radius)
}

@group(0) @binding(3) var<storage, read> pointLights: array<PointLight>;
@group(0) @binding(4) var<storage, read> clusterRanges: array<vec2u>; // per cluster: first, count
@group(0) @binding(5) var<storage, read> clusterLights: array<u32>;   // indices into pointLights
// The shadowed lights' cube shadow maps: each texel how far the nearest
// surface in its direction is from the light, / the light's reach (see
// meshPointShadow.wgsl).
@group(0) @binding(6) var pointShadowMaps: texture_depth_cube_array;

// How much of a shadowed light reaches a point (0 to 1): its distance from
// the light against the nearest surface's in that direction, softened over a
// few texels. 1 for a light without shadows.
fn pointShadow(light: PointLight, worldPosition: vec3f, geometryNormal: vec3f) -> f32 {
  let layer = i32(light.shadow.x);
  if (layer < 0) {
    return 1.0;
  }
  let center = light.positionRadius.xyz;
  // A texel there is about this wide (a face spans 90 degrees): step off the
  // surface by a little more, so it doesn't shadow itself.
  let texel = 2.0 * distance(worldPosition, center) * light.shadow.y;
  let toPoint = worldPosition + geometryNormal * (texel * 1.5) - center;
  let reference = length(toPoint) / light.positionRadius.w - 0.002;

  // Five taps: the direction, and a texel either way across it.
  let direction = normalize(toPoint);
  let helper = select(vec3f(0.0, 0.0, 1.0), vec3f(1.0, 0.0, 0.0), abs(direction.z) > 0.9);
  let across = normalize(cross(direction, helper)) * (2.0 * light.shadow.y);
  let up = cross(direction, across);
  var lit = textureSampleCompareLevel(pointShadowMaps, shadowSampler, direction, layer, reference);
  lit += textureSampleCompareLevel(pointShadowMaps, shadowSampler, direction + across, layer, reference);
  lit += textureSampleCompareLevel(pointShadowMaps, shadowSampler, direction - across, layer, reference);
  lit += textureSampleCompareLevel(pointShadowMaps, shadowSampler, direction + up, layer, reference);
  lit += textureSampleCompareLevel(pointShadowMaps, shadowSampler, direction - up, layer, reference);
  return lit / 5.0;
}

// The cluster a pixel (framebuffer position) at a world position is in.
fn clusterOf(pixel: vec2f, worldPosition: vec3f) -> u32 {
  let size = frame.clusterSize;
  let depthRange = frame.clusterDepth;
  let tile = min(vec2u(floor(pixel / depthRange.w)), size.xy - vec2u(1u));
  let depth = dot(worldPosition - frame.cameraPosition.xyz, frame.cameraForward.xyz);
  let slice = clamp(floor(log(max(depth, depthRange.x) / depthRange.x) * depthRange.z), 0.0,
                    f32(size.z - 1u));
  return (u32(slice) * size.y + tile.y) * size.x + tile.x;
}

// What the point lights do to a point: its diffuse light, in the isometric
// renderer's style, and their highlights (see pointLighting).
struct PointLit {
  diffuse: vec3f,
  specular: vec3f,
}

// The surface a point light lights, for its highlights.
struct Shine {
  toView: vec3f,
  f0: vec3f,         // reflectance face-on
  roughness: f32,
  reflectivity: f32, // how much of it reflects at all (as the sun's glint)
}

// Point-light contribution. Diffuse: each light adds a smootherstep-
// attenuated, softened diffuse term; colours blend by weight, and the total
// brightness of the lights reaching a point adds up, softly capped so
// overlapping lamps don't blow out. Both are cut by the light's shadows
// (pointShadow), for lights casting them. Specular: each light's GGX highlight, as
// bright as diffuse light of the same strength would be (x pi: the diffuse
// term has no 1 / pi), so metal and glass, which have little or no diffuse
// colour, show the lights by reflecting them. Only the lights listed in the
// pixel's cluster are looked at.
fn pointLighting(pixel: vec2f, worldPosition: vec3f, normal: vec3f, geometryNormal: vec3f,
                 shine: Shine) -> PointLit {
  var result: PointLit;
  if (frame.clusterSize.w == 0u) {
    return result;
  }
  var weightedColor = vec3f(0.0);
  var totalWeight = 0.0;
  var specular = vec3f(0.0);

  let range = clusterRanges[clusterOf(pixel, worldPosition)];
  for (var k = 0u; k < range.y; k++) {
    let index = clusterLights[range.x + k];
    let light = pointLights[index];
    let toLight = light.positionRadius.xyz - worldPosition;
    let dist = length(toLight);
    let radius = light.positionRadius.w;
    if (dist >= radius) {
      continue;
    }

    let reach = clamp(1.0 - dist / radius, 0.0, 1.0);
    let attenuation = reach * reach * reach * (reach * (reach * 6.0 - 15.0) + 10.0);
    let direction = toLight / max(dist, 1e-4);
    let ndotl = max(dot(normal, direction), 0.0);
    let diffuse = mix(0.12, 1.0, pow(ndotl, 0.8));
    let strength = light.colorIntensity.w * attenuation * pointShadow(light, worldPosition, geometryNormal);
    let amount = strength * diffuse;

    var color = light.colorIntensity.rgb;
    let maxChannel = max(max(color.r, color.g), color.b);
    color = select(vec3f(1.0), color / maxChannel, maxChannel > 0.001);

    weightedColor += color * amount;
    totalWeight += amount;
    specular += sphereSpecular(normal, shine, toLight, dist, light.shadow.z) * color * (strength * PI);
  }

  if (totalWeight > 0.001) {
    let capped = (totalWeight / (1.0 + totalWeight)) * 1.65;
    result.diffuse = (weightedColor / totalWeight) * capped;
  }
  result.specular = specular * shine.reflectivity;
  return result;
}

// A light glinting off the surface (GGX), from direction `toLight`: sharp on
// smooth surfaces, broad and faint on rough ones. Includes n.l. Unbounded: a
// mirror's peak is huge, so callers cap it (after any normalization).
fn ggxSpecular(normal: vec3f, toView: vec3f, toLight: vec3f, f0: vec3f, roughness: f32) -> vec3f {
  let ndotl = max(dot(normal, toLight), 0.0);
  if (ndotl <= 0.0) {
    return vec3f(0.0);
  }
  let halfway = normalize(toLight + toView);
  let ndoth = max(dot(normal, halfway), 0.0);
  let ndotv = max(dot(normal, toView), 1e-3);
  let a = roughness * roughness;
  let a2 = a * a;
  let d = ndoth * ndoth * (a2 - 1.0) + 1.0;
  let distribution = a2 / (PI * d * d);
  let k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
  let geometry = (ndotl / (ndotl * (1.0 - k) + k)) * (ndotv / (ndotv * (1.0 - k) + k));
  let fresnel = f0 + (1.0 - f0) * pow(1.0 - max(dot(toView, halfway), 0.0), 5.0);
  let specular = distribution * geometry * fresnel / (4.0 * ndotl * ndotv + 1e-4);
  return specular * ndotl;
}

// A light `sourceRadius` big glinting off the surface, from `toLight` (its
// centre, `dist` away): GGX toward the point of it nearest the reflected view
// direction, so a mirror shows its whole size and not a pinpoint, dimmed by
// how much wider than the surface's own the highlight now is (Karis's
// sphere-light approximation, as in Unreal).
fn sphereSpecular(normal: vec3f, shine: Shine, toLight: vec3f, dist: f32, sourceRadius: f32) -> vec3f {
  if (sourceRadius <= 0.0) {
    return min(ggxSpecular(normal, shine.toView, toLight / max(dist, 1e-4), shine.f0, shine.roughness),
               vec3f(8.0));
  }
  let reflected = reflect(-shine.toView, normal);
  let centerToRay = dot(toLight, reflected) * reflected - toLight;
  let nearest = toLight + centerToRay * clamp(sourceRadius / max(length(centerToRay), 1e-4), 0.0, 1.0);
  let a = shine.roughness * shine.roughness;
  let widened = clamp(a + sourceRadius / (2.0 * max(dist, 1e-4)), 0.0, 1.0);
  let spread = (a / widened) * (a / widened);
  return min(ggxSpecular(normal, shine.toView, normalize(nearest), shine.f0, shine.roughness) * spread,
             vec3f(8.0));
}

// What a shiny surface reflects: the sky above (paling to a bright horizon)
// and darker ground below. Rougher surfaces reflect along a direction pulled
// toward their normal, with a softer horizon: blurred, but still bright above
// and dark below, which is what makes rough metal read as metal.
fn environment(normal: vec3f, toView: vec3f, roughness: f32) -> vec3f {
  let sky = frame.sky.rgb;
  let horizon = mix(sky, vec3f(1.0), 0.55);
  let ground = sky * 0.3 + vec3f(0.06);
  let reflected = reflect(-toView, normal);
  let up = normalize(mix(reflected, normal, roughness * roughness)).z;
  let softness = mix(0.05, 0.55, roughness);
  let above = mix(horizon, sky, smoothstep(0.0, 0.7, up));
  return mix(ground, above, smoothstep(-softness, softness * 0.5, up));
}

// The lit colour, and the light of it that blooms: the same, x the
// material's Bloom (post.wgsl's prefilter reads it).
struct LitOut {
  @location(0) color: vec4f,
  @location(1) glow: vec4f,
}

// How hazed a point is (see HazeSettings), 0 to its strength.
fn hazeAt(worldPosition: vec3f) -> f32 {
  let away = distance(worldPosition, frame.cameraPosition.xyz) * frame.haze.y;
  return frame.haze.x * (1.0 - exp2(-away * away));
}

@fragment
fn fs_main(in: VertexOut) -> LitOut {
  return shade(in);
}

// A surface lit, as the lit pass's fragment stage (others build on it, as
// grass's ground does).
fn shade(in: VertexOut) -> LitOut {
  // The surface first, where control flow is still uniform (it may sample
  // textures).
  let geometryNormal = normalize(in.normal);
  var here = surfaceInput(in.uv, in.worldPosition, geometryNormal, in.vertexColor);
  here.grass = in.grass;
  here.grassData = in.grassData;
  let s = surface(here);
  // The surface's (tangent-space) normal, along the point's tangent frame.
  let normal = normalize(here.tangent * s.normal.x + here.bitangent * s.normal.y + geometryNormal * s.normal.z);

  // Opaque things take only the object's tint alpha.
  let mode = surfaceAlphaMode();
  var alpha = select(s.alpha * in.tint.a, in.tint.a, mode == ALPHA_OPAQUE);
  if (mode == ALPHA_MASK) {
    if (alpha < s.alphaClip) {
      discard;
    }
    alpha = in.tint.a; // what's left is solid
  }

  let albedo = s.baseColor * in.tint.rgb;
  let metallic = clamp(s.metallic, 0.0, 1.0);
  let roughness = clamp(s.roughness, 0.04, 1.0);

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
  // The shadow lookup follows the mesh's own surface, not its bumps.
  let visibility = sunVisibility(in.worldPosition, geometryNormal);
  lit = mix(shadedLevel, lit, visibility);

  let sunlight = max(vec3f(lit) * frame.sunColor.rgb, vec3f(0.03));

  // How the surface reflects (below), which point lights' highlights need.
  let toView = normalize(frame.cameraPosition.xyz - in.worldPosition);
  let f0 = mix(vec3f(0.04), albedo, metallic);
  let reflectivity = mix(1.0 - roughness, 1.0, metallic);

  // Point lights only brighten what sunlight hasn't already: by day the
  // shade (sunlit ground is full already), at night everything.
  let daylight = smoothstep(0.20, 0.75, ambient);
  let pointLit = pointLighting(in.clip.xy, in.worldPosition, normal, geometryNormal,
                               Shine(toView, f0, roughness, reflectivity));
  let point = pointLit.diffuse * 2.0;

  let sunlightAmount = max(max(sunlight.r, sunlight.g), sunlight.b);
  let shadowRoom = mix(1.0, 1.0 - smoothstep(0.65, 1.0, sunlightAmount), daylight);

  let total = clamp(sunlight + point * shadowRoom, vec3f(0.0), vec3f(1.0));

  // Metal has no diffuse colour of its own: it shows what it reflects, tinted
  // by its albedo. Paint and plastic reflect a little, more when smooth (rough
  // ones not at all, so matte things keep the flat look above).
  let ndotv = max(dot(normal, toView), 1e-3);
  let fresnel = f0 + (max(vec3f(1.0 - roughness), f0) - f0) * pow(1.0 - ndotv, 5.0);
  // As bright as the scene's light: ambient plus some of the sun (dim at night).
  let reflection = environment(normal, toView, roughness) * fresnel * reflectivity *
                   mix(0.6, 1.0, visibility) * (ambient + 0.5 * diffuseStrength);
  let glint = min(ggxSpecular(normal, toView, toSun, f0, roughness), vec3f(8.0)) * frame.sunColor.rgb *
                  (diffuseStrength * visibility * reflectivity) +
              pointLit.specular * shadowRoom;

  // Emission is light of its own: unaffected by the scene's.
  let lightColor = albedo * (1.0 - metallic) * total + reflection + glint + max(s.emission, vec3f(0.0)) * in.tint.rgb;
  // Into the haze, toward the sky (glowing less, the more it's hidden).
  let haze = hazeAt(in.worldPosition);
  let color = mix(lightColor, frame.sky.rgb, haze);
  return LitOut(vec4f(color, alpha), vec4f(lightColor * max(s.bloom, 0.0) * (1.0 - haze), alpha));
}
