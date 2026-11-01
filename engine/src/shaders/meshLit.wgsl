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
  @location(5) grass: vec4f,     // see SurfaceIn, and w how shut in by grass (others: 1, 0, 0, 0)
  @location(6) grassData: vec4f, // see SurfaceIn
  // Grass blades: how wet (x, surfaceWetness()) and dripped on (y,
  // dripLineWetness()), found per vertex, as they're thin and drawn over each
  // other (others: x below 0, found at each point).
  @location(7) rain: vec2f,
}

// A physical sky's (see skyCommon.wgsl and sky.wgsl): its reflections, sharp
// (mip 0) to rough; the sampler; the sky itself (what haze fades into); and
// the split-sum BRDF table (r scale, g bias on f0; by n.v across, roughness
// down).
@group(0) @binding(7) var skyReflections: texture_2d<f32>;
@group(0) @binding(8) var skySampler: sampler;
@group(0) @binding(9) var skyRadiance: texture_2d<f32>;
@group(0) @binding(10) var brdfTable: texture_2d<f32>;

// Ambient occlusion and contact shadows (see ambientOcclusion.wgsl), at half
// the screen's size: r how open each point is (1: fully), g its depth along
// the view, b how much of the sun's light reaches it past what's just in
// front of it. A 1x1 stand-in while both are off.
@group(0) @binding(11) var occlusionMap: texture_2d<f32>;

// Rain's overhead map (see RainMap): depth looking straight down over the
// ground ahead of the camera (0 at frame.rainDepth.x, 1 that far below), of
// the sun's shadow casters (and the opaque things in view that don't cast:
// the ground), so whatever has something drawn above it is
// sheltered from the rain. Read only while frame.rainMap.w is 1 (else a 1x1
// stand-in).
@group(0) @binding(12) var rainMap: texture_depth_2d;

// Where a world XY is on rain's overhead map (uv; outside 0..1: off it).
fn rainMapUv(p: vec2f) -> vec2f {
  let local = (p - frame.rainMap.xy) / frame.rainMap.z;
  return vec2f(0.5 + 0.5 * local.x, 0.5 - 0.5 * local.y);
}

// The top of what's drawn at a world XY, from the overhead map (very low:
// nothing there, off the map, or no map).
fn rainSurfaceHeight(p: vec2f) -> f32 {
  let uv = rainMapUv(p);
  if (frame.rainMap.w < 0.5 || any(uv <= vec2f(0.0)) || any(uv >= vec2f(1.0))) {
    return -1e9;
  }
  let size = vec2i(textureDimensions(rainMap));
  let depth = textureLoad(rainMap, clamp(vec2i(uv * vec2f(size)), vec2i(0), size - vec2i(1)), 0);
  if (depth >= 1.0) {
    return -1e9;
  }
  return frame.rainDepth.x - depth * frame.rainDepth.y;
}

// How open to the rain a point is (0: sheltered, under a roof or a tree; 1:
// open to the sky): whether anything on the overhead map is above it,
// looked for a little off the surface (so a wall looks beside itself, not at
// its own top) and softened over a few texels. Open off the map, or while
// it isn't drawn.
fn rainExposure(worldPosition: vec3f, geometryNormal: vec3f) -> f32 {
  if (frame.rainMap.w < 0.5) {
    return 1.0;
  }
  let p = worldPosition + geometryNormal * 0.12 + vec3f(0.0, 0.0, 0.05);
  let uv = rainMapUv(p.xy);
  if (any(uv <= vec2f(0.0)) || any(uv >= vec2f(1.0))) {
    return 1.0;
  }
  let depth = (frame.rainDepth.x - p.z) / frame.rainDepth.y;
  let texel = 1.5 / f32(textureDimensions(rainMap).x);
  var open = 0.0;
  for (var i = 0; i < 4; i++) {
    let corner = vec2f(f32(i & 1), f32(i >> 1u)) * 2.0 - vec2f(1.0);
    open += textureSampleCompareLevel(rainMap, shadowSampler, uv + corner * texel, depth);
  }
  // Toward the map's edges, open (as off it).
  let edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
  return mix(1.0, open * 0.25, smoothstep(0.0, 0.05, edge));
}

// How wet a surface is (0..1): the weather's wetness where the rain reaches
// it: all of it facing up, about a third on walls (rain driven against
// them), none facing down or sheltered.
fn surfaceWetness(worldPosition: vec3f, geometryNormal: vec3f) -> f32 {
  let wetness = frame.weather.y;
  if (wetness <= 0.0) {
    return 0.0;
  }
  let up = geometryNormal.z;
  let facing = 0.35 * smoothstep(-0.5, 0.0, up) + 0.65 * max(up, 0.0);
  return wetness * facing * rainExposure(worldPosition, geometryNormal);
}

// How wet the ground is along a drip line (0..1, frame.rainDrips.w): open
// to the rain itself, but within a quarter of a metre of an edge standing
// well above it (an eave, a canopy's rim) that water drips off: soaked, with
// water standing on it (half as much again). None while there are no drips.
fn dripLineWetness(worldPosition: vec3f, geometryNormal: vec3f) -> f32 {
  if (frame.rainDrips.w <= 0.0 || frame.rainMap.w < 0.5 || geometryNormal.z < 0.7) {
    return 0.0;
  }
  if (rainSurfaceHeight(worldPosition.xy) > worldPosition.z + 0.1) {
    return 0.0;
  }
  var under = 0.0;
  for (var i = 0; i < 4; i++) {
    let a = f32(i) * 1.5707963;
    let h = rainSurfaceHeight(worldPosition.xy + vec2f(cos(a), sin(a)) * 0.25);
    under += smoothstep(0.6, 1.0, h - worldPosition.z);
  }
  return frame.rainDrips.w * min(under, 1.0);
}

// How much water stands on a surface (0..1), as wetMaterial() takes it: a
// film on soaked flat ground. (A terrain's puddles add theirs, Surface's
// water, in shade().)
fn standingWater(worldPosition: vec3f, geometryNormal: vec3f, wet: f32) -> f32 {
  let level = smoothstep(0.92, 0.99, geometryNormal.z);
  return 0.35 * level * smoothstep(0.6, 1.0, wet);
}

// A surface's material, wet: what shade() lights.
struct WetMaterial {
  albedo: vec3f,
  roughness: f32,
  normal: vec3f,
}

// A surface `wet` (0..1, surfaceWetness()) with `water` standing on it
// (0..1, standingWater()), after Lagarde's physically based wet surfaces:
// porous things (rough, not metal: soil, wood, stone, cloth) soak the water
// up and darken (light is trapped in their wet pores, to half the albedo),
// and all of them get glossier, less so the more porous; standing water is a
// smooth layer over it (its normal the geometry's), rings spreading on it
// where drops land (rainRipples()), if `rings`. Smooth, flat, up-facing
// dielectrics (water: a river, a pond) ring in the rain as standing water
// does, wet or not.
fn wetMaterial(albedo: vec3f, metallic: f32, roughness: f32, normal: vec3f, geometryNormal: vec3f,
               worldPosition: vec3f, wet: f32, water: f32, rings: bool) -> WetMaterial {
  var out = WetMaterial(albedo, roughness, normal);
  let level = smoothstep(0.92, 0.99, geometryNormal.z);
  let waterLike = level * (1.0 - metallic) * (1.0 - smoothstep(0.08, 0.2, roughness));
  if (wet <= 0.0 && water <= 0.0 && (waterLike <= 0.0 || frame.weather.x <= 0.0)) {
    return out;
  }
  let porosity = clamp((roughness - 0.35) / 0.45, 0.0, 1.0) * (1.0 - metallic);
  let soaked = mix(1.0, 0.5, porosity);
  out.albedo = albedo * mix(1.0, soaked, max(wet, water));
  out.roughness = roughness * mix(1.0, mix(0.7, 0.55, porosity), wet);
  out.roughness = mix(out.roughness, 0.05, water);
  out.normal = normalize(mix(normal, geometryNormal, water));

  let ringing = max(water, waterLike) * frame.rainStreaks.w;
  if (rings && frame.weather.x > 0.0 && ringing > 0.0) {
    let slope = rainRipples(worldPosition.xy, frame.time.x, frame.weather.x) * ringing *
                rainExposure(worldPosition, geometryNormal);
    out.normal = normalize(out.normal - vec3f(slope, 0.0));
  }
  return out;
}

// How open a point drawn at `pixel` (framebuffer) is to the light from all
// around (x), and how much of the sun's light its contact shadows leave it
// (y): the occlusion map's four texels around it, weighted by how near each
// is (bilinearly) and how near in depth to the point (so they don't bleed
// across edges). 1 when they're off, or none of them is at its depth (things
// it doesn't hold: grass blades, see-through things).
fn screenOcclusionAt(pixel: vec2f, worldPosition: vec3f) -> vec2f {
  if (frame.ambientOcclusion.x < 0.5) {
    return vec2f(1.0);
  }
  let depth = dot(worldPosition - frame.cameraPosition.xyz, frame.cameraForward.xyz);
  let size = vec2i(textureDimensions(occlusionMap));
  let at = pixel * 0.5 - vec2f(0.5);
  let base = vec2i(floor(at));
  let f = at - floor(at);
  let tolerance = max(depth * frame.ambientOcclusion.y, 0.02);
  var sum = vec2f(0.0);
  var total = 0.0;
  for (var i = 0; i < 4; i++) {
    let corner = vec2i(i & 1, i >> 1u);
    let texel = textureLoad(occlusionMap, clamp(base + corner, vec2i(0), size - vec2i(1)), 0);
    let across = select(1.0 - f.x, f.x, corner.x == 1);
    let down = select(1.0 - f.y, f.y, corner.y == 1);
    let weight = (across * down + 0.01) * max(1.0 - abs(texel.g - depth) / tolerance, 0.0);
    sum += texel.rb * weight;
    total += weight;
  }
  if (total < 1e-4) {
    return vec2f(1.0);
  }
  return sum / total;
}

// Sun visibility: 1 = lit, 0 = fully in shadow. A grid of hardware-filtered
// comparison taps, spread by `softness` texels, gives soft edges.
fn sunVisibility(worldPosition: vec3f, normal: vec3f) -> f32 {
  if (frame.shadow.x < 0.5) {
    return 1.0;
  }
  if (frame.cascadeInfo.x > 1.5) {
    return cascadedVisibility(worldPosition, normal);
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

// One cascade's look at a point: x its visibility, y how far inside the
// cascade it is (0 at its edge, 1 at its centre; below 0: outside it).
fn cascadeTap(cascade: u32, worldPosition: vec3f, normal: vec3f) -> vec2f {
  let params = frame.cascadeParams[cascade];
  // Off the surface by a texel and a half of this cascade's, as the one map.
  let offsetPosition = worldPosition + normal * (params.x * 1.5);
  let lightClip = frame.cascades[cascade] * vec4f(offsetPosition, 1.0);
  let ndc = lightClip.xyz / lightClip.w;
  let inside = 1.0 - max(abs(ndc.x), abs(ndc.y));
  if (inside <= 0.0 || ndc.z > 1.0 || ndc.z < 0.0) {
    return vec2f(1.0, -1.0);
  }
  // Into its tile of the atlas, the filter's taps kept within it.
  let rect = frame.cascadeRects[cascade];
  let margin = params.zw / rect.zw * 1.5 + vec2f(1e-4);
  let local = clamp(vec2f(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5), margin, vec2f(1.0) - margin);
  let uv = rect.xy + local * rect.zw;
  let depth = ndc.z - params.y;
  var sum = 0.0;
  for (var y = -1; y <= 1; y++) {
    for (var x = -1; x <= 1; x++) {
      let tap = uv + vec2f(f32(x), f32(y)) * params.zw;
      sum += textureSampleCompareLevel(shadowMap, shadowSampler, tap, depth);
    }
  }
  return vec2f(sum / 9.0, inside);
}

// Sun visibility from cascades: the finest whose box holds the point,
// blended into the next over its edge, fading out at the far end.
fn cascadedVisibility(worldPosition: vec3f, normal: vec3f) -> f32 {
  let count = u32(frame.cascadeInfo.x);
  let band = frame.cascadeInfo.y;
  let along = dot(worldPosition - frame.cameraPosition.xyz, frame.cameraForward.xyz);
  let fade = smoothstep(frame.cascadeInfo.z, frame.cascadeInfo.w, along);
  if (fade >= 1.0) {
    return 1.0;
  }
  for (var c = 0u; c < count; c++) {
    let tap = cascadeTap(c, worldPosition, normal);
    if (tap.y < 0.0) {
      continue;
    }
    var visibility = tap.x;
    if (tap.y < band && c + 1u < count) {
      let next = cascadeTap(c + 1u, worldPosition, normal);
      if (next.y >= 0.0) {
        visibility = mix(next.x, tap.x, tap.y / band);
      }
    }
    return mix(visibility, 1.0, fade);
  }
  return 1.0;
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
  // Under a physical sky, a lamp's shadow fades out as daylight drowns the
  // lamp out (PhysicalSky::lampShadow), until it's no longer drawn.
  var shows = 1.0;
  if (frame.skyMode.x > 0.5) {
    shows = smoothstep(0.2, 0.4, light.colorIntensity.w * frame.skyLight.z);
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
  return mix(1.0, lit / 5.0, shows);
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

// A physical sky's diffuse light on a surface facing `normal` (its SH9).
fn skyIrradianceAt(normal: vec3f) -> vec3f {
  let n = normal;
  let sh = frame.skyIrradiance;
  let light = sh[0].rgb * 0.282095 +
              (sh[1].rgb * n.y + sh[2].rgb * n.z + sh[3].rgb * n.x) * 0.488603 +
              (sh[4].rgb * (n.x * n.y) + sh[5].rgb * (n.y * n.z) + sh[7].rgb * (n.x * n.z)) * 1.092548 +
              sh[6].rgb * (0.315392 * (3.0 * n.z * n.z - 1.0)) +
              sh[8].rgb * (0.546274 * (n.x * n.x - n.y * n.y));
  return max(light, vec3f(0.0));
}

// What a surface of `roughness` reflects of a physical sky from `direction`
// (before its reflections are rendered: its diffuse light, as a blur).
fn skyReflection(direction: vec3f, roughness: f32) -> vec3f {
  if (frame.skyMode.y < 0.5) {
    return skyIrradianceAt(direction);
  }
  return textureSampleLevel(skyReflections, skySampler, octEncode(direction), roughness * frame.skyMode.z).rgb *
         frame.skyLight.x;
}

// The sky at the horizon behind a point: what haze fades it into, under a
// physical sky (as the background shows it).
fn hazeSky(worldPosition: vec3f) -> vec3f {
  let away = worldPosition - frame.cameraPosition.xyz;
  let level = normalize(vec3f(away.xy, max(away.z, 0.0) + 0.05 * length(away.xy) + 1e-4));
  if (frame.skyMode.y < 0.5) {
    return skyIrradianceAt(level) * frame.skyScale.rgb;
  }
  return overcastHaze(textureSampleLevel(skyRadiance, skySampler, octEncode(level), 0.0).rgb) * frame.skyScale.rgb;
}

// How much of the sun's (or the moon's) light a physical sky's clouds let
// reach a point, x frame.cloudBase.w (sunLight is dimmed by their average:
// this puts it back where they aren't, and takes it where they are): their
// layer where the light's line to the point crosses it. 1 without clouds.
fn cloudShadowAt(worldPosition: vec3f) -> f32 {
  if (frame.cloudBase.w <= 0.0) {
    return 1.0;
  }
  let toLight = frame.sunDirection.xyz;
  let rise = CLOUD_ALTITUDE * 1000.0 - worldPosition.z;
  let slant = toLight.xy / max(toLight.z, 0.08);
  let p = (worldPosition.xy + slant * rise) * 0.001 + frame.weatherWind.zw;
  // A closing deck has no gaps left to let the sun through (as clearSky()).
  let deck = smoothstep(0.75, 1.0, frame.weather.w);
  let density = mix(cloudDensityAt(p, frame.weather.w, 4), 1.0, deck);
  let blocked = (0.85 + 0.12 * frame.cloudLight.w) * density;
  return (1.0 - blocked) * (1.0 - 0.9 * deck) * frame.cloudBase.w;
}

// A lightning strike's light on a surface (under a physical sky; see
// LightningStrike): its channel's, a line of light from the ground to the
// cloud base (the inverse square summed along it: about 1 / the distance
// near it, 1 / its square far off), from where along it its light comes
// from on average, as Lambert diffuse and a GGX glint (wet ground's sharp),
// shadowed by its point light's cube where it has one; and the glowing cloud
// base's, from above, softly.
struct StrikeLit {
  diffuse: vec3f,
  specular: vec3f,
}

fn lightningLighting(worldPosition: vec3f, normal: vec3f, geometryNormal: vec3f, toView: vec3f, f0: vec3f,
                     roughness: f32, occlusion: f32) -> StrikeLit {
  var result: StrikeLit;
  if (frame.lightningGlow.w <= 0.0) {
    return result;
  }
  let glow = lightningGlowAt(worldPosition);
  result.diffuse = glow * ((0.2 + 0.4 * max(normal.z, 0.0)) * occlusion);
  if (frame.lightningHigh.w < 0.5) {
    return result;
  }
  let low = frame.lightningLow.xyz;
  let line = frame.lightningHigh.xyz - low;
  let span = max(sqrt(dot(line, line)), 1e-3);
  let up = line / span;
  let to = worldPosition - low;
  let s = dot(to, up);
  let h = max(sqrt(max(dot(to, to) - s * s, 0.0)), 1.0);
  let a = -s;
  let b = span - s;
  let weight = (atan(b / h) - atan(a / h)) / h;
  let moment = 0.5 * log((h * h + b * b) / (h * h + a * a));
  let centre = low + up * clamp(s + moment / max(weight, 1e-12), 0.0, span);
  let toLight = normalize(centre - worldPosition);
  var visible = 1.0;
  let index = i32(frame.lightningLow.w);
  if (index >= 0) {
    let caster = pointLights[u32(index)];
    // Within its cube's reach (past it, unshadowed).
    if (distance(worldPosition, caster.positionRadius.xyz) < caster.positionRadius.w * 0.98) {
      visible = pointShadow(caster, worldPosition, geometryNormal);
    }
  }
  let light = frame.lightningLight.rgb * (weight * visible);
  result.diffuse += light * max(dot(normal, toLight), 0.0);
  result.specular = min(ggxSpecular(normal, toView, toLight, f0, roughness), vec3f(8.0)) * light * PI;
  return result;
}

// A surface lit by a physical sky: the sun's light (through the air; none
// once it's set) as Lambert diffuse and a GGX glint, the sky's diffuse light
// (its SH9) and its reflections (prefiltered, split sum), and point lights
// as under a flat sky, which with emission eyes adapted to daylight see
// dimmer (frame.skyLight.z). A visual style scales the sun's light and the sky's
// (diffuse and reflected) by its balance. Contact shadows (`contact`, the
// sun's light they leave) take only the sun's, as its shadow maps and the
// clouds' shadows do.
fn shadePhysical(in: VertexOut, s: Surface, normal: vec3f, geometryNormal: vec3f, albedo: vec3f,
                 metallic: f32, roughness: f32, alpha: f32, occlusion: f32, contact: f32) -> LitOut {
  let toSun = frame.sunDirection.xyz;
  let toView = normalize(frame.cameraPosition.xyz - in.worldPosition);
  let visibility = sunVisibility(in.worldPosition, geometryNormal) * contact * cloudShadowAt(in.worldPosition);
  let sun = frame.sunLight.rgb * frame.sunBalance.rgb;
  let ambient = frame.ambientBalance.rgb;
  let f0 = mix(vec3f(0.04), albedo, metallic);

  let direct = sun * (max(dot(normal, toSun), 0.0) * visibility);
  let skyLight = skyIrradianceAt(normal) * ambient * occlusion;

  // Point lights and emission are as bright as they are, lit for eyes
  // adapted to the night: by day those eyes see them dimmer (x lamp), and
  // daylight drowns all but strong ones out.
  let lamp = frame.skyLight.z;
  let pointLit = pointLighting(in.clip.xy, in.worldPosition, normal, geometryNormal,
                               Shine(toView, f0, roughness, 1.0));
  let strike = lightningLighting(in.worldPosition, normal, geometryNormal, toView, f0, roughness, occlusion);
  let diffuse = min(direct + skyLight + pointLit.diffuse * (2.0 * lamp) + strike.diffuse,
                    vec3f(frame.ambientBalance.w));

  // The sky's reflection, hidden where it would come from below the
  // surface's own horizon, and the sun's glint.
  let ndotv = max(dot(normal, toView), 1e-3);
  let brdf = textureSampleLevel(brdfTable, skySampler, vec2f(ndotv, roughness), 0.0).rg;
  let reflected = reflect(-toView, normal);
  let horizon = clamp(1.0 + dot(reflected, geometryNormal), 0.0, 1.0);
  // (The cloud base flashing shows in it too, as the sky's reflections
  // can't keep up with: cheaply, where the reflection meets the clouds.)
  let flash = lightningCloudsToward(in.worldPosition, normalize(mix(reflected, normal, roughness * roughness)));
  let reflection = (skyReflection(reflected, roughness) * ambient + flash) * (f0 * brdf.x + brdf.y) *
                   (horizon * horizon) * occlusion;
  let glint = min(ggxSpecular(normal, toView, toSun, f0, roughness), vec3f(8.0)) * sun * (PI * visibility) +
              pointLit.specular * lamp + strike.specular;

  let lightColor = albedo * (1.0 - metallic) * diffuse + reflection + glint +
                   max(s.emission, vec3f(0.0)) * (in.tint.rgb * lamp);
  let haze = hazeAt(in.worldPosition);
  var color = mix(lightColor, hazeSky(in.worldPosition), haze);
  var clearness = 1.0 - haze;
  if (frame.fogLight.z > 0.5) {
    let fogged = fogAt(in.worldPosition);
    color = mix(color, fogColor(in.worldPosition - frame.cameraPosition.xyz), fogged);
    clearness *= 1.0 - fogged;
  }
  let veiled = rainVeilAt(in.worldPosition);
  color = mix(color, fogColor(in.worldPosition - frame.cameraPosition.xyz), veiled);
  clearness *= 1.0 - veiled;
  return LitOut(vec4f(color, alpha), vec4f(lightColor * max(s.bloom, 0.0) * clearness, alpha));
}

// The colour of height fog toward `away` (from the camera): under a physical
// sky, the sky at the horizon that way (as haze), glowing toward the sun; under
// a flat one, the sky colour.
fn fogColor(away: vec3f) -> vec3f {
  if (frame.skyMode.x < 0.5) {
    return frame.sky.rgb;
  }
  let direction = normalize(away);
  // (Lit up by lightning's glowing clouds, as the air is: the veil of rain,
  // the haze toward the horizon.)
  return hazeSky(frame.cameraPosition.xyz + away) + fogSunGlow(direction) +
         lightningGlowAt(frame.cameraPosition.xyz + away * 0.5) * 0.3;
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
fn fs_main(in: VertexOut, @builtin(front_facing) front: bool) -> LitOut {
  // A back face (drawn only for a double-sided material) is lit as seen.
  var seen = in;
  seen.normal = select(-in.normal, in.normal, front);
  return shade(seen);
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
  let bumped = normalize(here.tangent * s.normal.x + here.bitangent * s.normal.y + geometryNormal * s.normal.z);

  // Opaque things take only the object's tint alpha.
  let mode = surfaceAlphaMode();
  var alpha = select(s.alpha * in.tint.a, in.tint.a, mode == ALPHA_OPAQUE);
  if (mode == ALPHA_MASK) {
    if (alpha < s.alphaClip) {
      discard;
    }
    alpha = in.tint.a; // what's left is solid
  }

  let metallic = clamp(s.metallic, 0.0, 1.0);
  // Wet from the rain: darker, glossier, and where water stands on it
  // smooth and rippled.
  // A terrain's puddles: water standing in its low spots, soaked ground
  // around them.
  // Under an edge's drips, soaked and puddled.
  // (A grass blade's found per vertex, and too thin to show rings.)
  let perPoint = in.rain.x < 0.0;
  var dripped = in.rain.y;
  var weathered = in.rain.x;
  if (perPoint) {
    dripped = dripLineWetness(in.worldPosition, geometryNormal);
    weathered = surfaceWetness(in.worldPosition, geometryNormal);
  }
  let wet = max(max(weathered, s.soak), dripped);
  let water = max(max(standingWater(in.worldPosition, geometryNormal, wet), s.water), dripped * 0.5);
  let wetted = wetMaterial(s.baseColor * in.tint.rgb, metallic, clamp(s.roughness, 0.04, 1.0), bumped,
                           geometryNormal, in.worldPosition, wet, water, perPoint);
  let albedo = wetted.albedo;
  let roughness = clamp(wetted.roughness, 0.04, 1.0);
  let normal = wetted.normal;
  // How open it is to the light from all around (ambient occlusion), and
  // how much of the sun's its contact shadows leave it.
  // Down in grass, the blades around a point shut out the sky's light, and
  // some of the sun's (in.grass.w, see meshGrass.wgsl; 0 elsewhere).
  let screen = screenOcclusionAt(in.clip.xy, in.worldPosition);
  let shut = clamp(in.grass.w, 0.0, 1.0);
  let occlusion = screen.x * (1.0 - shut);
  let sunShut = 1.0 - shut * 0.5;
  if (frame.skyMode.x > 0.5) {
    return shadePhysical(in, s, normal, geometryNormal, albedo, metallic, roughness, alpha, occlusion,
                         screen.y * sunShut);
  }

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
  let visibility = sunVisibility(in.worldPosition, geometryNormal) * sunShut;
  lit = mix(shadedLevel, lit, visibility);

  var sunlight = max(vec3f(lit) * frame.sunColor.rgb, vec3f(0.03));
  // A visual style re-balances the light: the sun's own share (what shadow
  // and turning away take) and the ambient rest, each scaled and tinted.
  // Ambient occlusion darkens only the ambient rest, the same way.
  if (frame.sunBalance.w > 0.5 || occlusion < 1.0) {
    let direct = clamp(sunDiffuse * diffuseStrength * (1.0 - shade) * visibility, 0.0, max(lit, 0.0));
    sunlight = max((vec3f(lit - direct) * (frame.ambientBalance.rgb * occlusion) + vec3f(direct) * frame.sunBalance.rgb) *
                       frame.sunColor.rgb,
                   vec3f(0.03));
  }

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

  // Up to white, unless tone mapping rolls brighter light off.
  let total = clamp(sunlight + point * shadowRoom, vec3f(0.0), vec3f(frame.ambientBalance.w));

  // Metal has no diffuse colour of its own: it shows what it reflects, tinted
  // by its albedo. Paint and plastic reflect a little, more when smooth (rough
  // ones not at all, so matte things keep the flat look above).
  let ndotv = max(dot(normal, toView), 1e-3);
  let fresnel = f0 + (max(vec3f(1.0 - roughness), f0) - f0) * pow(1.0 - ndotv, 5.0);
  // As bright as the scene's light: ambient plus some of the sun (dim at night).
  let reflection = environment(normal, toView, roughness) * fresnel * reflectivity *
                   mix(0.6, 1.0, visibility) * (ambient + 0.5 * diffuseStrength) * occlusion;
  let glint = min(ggxSpecular(normal, toView, toSun, f0, roughness), vec3f(8.0)) * frame.sunColor.rgb * frame.sunBalance.rgb *
                  (diffuseStrength * visibility * reflectivity) +
              pointLit.specular * shadowRoom;

  // Emission is light of its own: unaffected by the scene's.
  let lightColor = albedo * (1.0 - metallic) * total + reflection + glint + max(s.emission, vec3f(0.0)) * in.tint.rgb;
  // Into the haze, toward the sky (glowing less, the more it's hidden).
  let haze = hazeAt(in.worldPosition);
  var color = mix(lightColor, frame.sky.rgb, haze);
  var clearness = 1.0 - haze;
  // Into height fog (in haze's place, when it's on).
  if (frame.fogLight.z > 0.5) {
    let fogged = fogAt(in.worldPosition);
    color = mix(color, fogColor(in.worldPosition - frame.cameraPosition.xyz), fogged);
    clearness *= 1.0 - fogged;
  }
  // Behind rain's veil.
  let veiled = rainVeilAt(in.worldPosition);
  color = mix(color, fogColor(in.worldPosition - frame.cameraPosition.xyz), veiled);
  clearness *= 1.0 - veiled;
  return LitOut(vec4f(color, alpha), vec4f(lightColor * max(s.bloom, 0.0) * clearness, alpha));
}
