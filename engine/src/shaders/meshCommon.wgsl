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
  weather: vec4f,      // the weather (SceneWeather), each 0..1: x rain, y wetness, z puddles, w cloud cover
  weatherWind: vec4f,  // xy the wind (m/s, world XY), zw how far the clouds have drifted (km)
  cloudLight: vec4f,   // a physical sky's clouds: rgb the light on their tops (the sun's at their
                       // height, or the moon's; not adapted), w how dark they are (0..1)
  cloudBase: vec4f,    // rgb their undersides lit through (not adapted), w 1 / the share of the sun's
                       // light they let through on average (sunLight is dimmed by it); 0: no clouds
  // Rain (RainSettings, while the weather has it; see rain.wgsl):
  rainView: mat4x4f,   // the overhead map's view-projection (straight down, z 0 at its top)
  rainMap: vec4f,      // xy its centre (world), z its half size, w 1 if it's drawn this frame
  rainDepth: vec4f,    // x its top (world Z), y how far down it reaches, z streaks, w splashes
  rainStreaks: vec4f,  // x a streak's length (m), y its width (m), z brightness, w how strongly
                       // drops ring water
  rainVeil: vec4f,     // x the veil's density (per metre), yz the viewport (pixels)
  rainDrift: vec4f,    // xy how far the drops have drifted on the wind (m), zw the wind they
                       // slant in (m/s, eased)
  rainDrips: vec4f,    // drips off edges: x how many places they're looked for, y the share that
                       // drip each round, z how much they trickle, w how wet the drip line is
  // Lightning (see LightningStrike; none while lightningGlow.w is 0), its light adapted:
  lightningLow: vec4f,   // xyz its channel's bottom (world), w the point light with its shadow (-1: none)
  lightningHigh: vec4f,  // xyz its top, where the cloud base glows, w 1 if the channel lights
  lightningLight: vec4f, // rgb a metre of the channel's light, w how wide the glow is (m)
  lightningGlow: vec4f,  // rgb the cloud base's glow at its middle, w 1 while it flashes
}

// A physical sky's cloud layer this frame (skyCommon.wgsl's CloudLayer), as
// seen from the camera.
fn frameClouds() -> CloudLayer {
  return CloudLayer(frame.cameraPosition.xy * 0.001 + frame.weatherWind.zw, frame.weather.w, frame.cloudLight.w,
                    frame.cloudLight.rgb, frame.cloudBase.rgb, frame.sunDirection.xyz);
}

// The clear sky toward the horizon (not adapted) as haze and fog take it on
// under clouds: paler, and greyer toward their light the more overcast.
fn overcastHaze(sky: vec3f) -> vec3f {
  if (frame.cloudBase.w <= 0.0) {
    return sky;
  }
  let layer = frameClouds();
  return mix(cloudySky(sky, layer.cover), cloudAverage(layer), layer.cover * layer.cover);
}

@group(0) @binding(0) var<uniform> frame: Frame;

// A lightning strike's glowing cloud base lighting a point from above (rgb,
// adapted): brightest under the middle of the glow, falling off past its
// width. 0 while nothing flashes.
fn lightningGlowAt(p: vec3f) -> vec3f {
  if (frame.lightningGlow.w <= 0.0) {
    return vec3f(0.0);
  }
  let away = p - frame.lightningHigh.xyz;
  let r = frame.lightningLight.w;
  return frame.lightningGlow.rgb * (r * r / (r * r + dot(away, away)));
}

// The cloud base lit up by a strike, seen from `origin` toward `direction`
// (rgb, adapted: the sky's light there brightened by it): where the ray
// meets the layer, near the flash, mottled by the clouds' pattern; faded far
// off as the clouds are. 0 below the layer's horizon, or with no flash.
fn lightningCloudsToward(origin: vec3f, direction: vec3f) -> vec3f {
  if (frame.lightningGlow.w <= 0.0 || direction.z <= 0.01) {
    return vec3f(0.0);
  }
  let rise = CLOUD_ALTITUDE * 1000.0 - origin.z;
  if (rise <= 0.0) {
    return vec3f(0.0);
  }
  let t = rise / direction.z;
  let q = origin + direction * t;
  let away = q.xy - frame.lightningHigh.xy;
  let r = frame.lightningLight.w;
  let spot = exp(-dot(away, away) / (r * r)) + 0.5 * r * r / (r * r + dot(away, away));
  let density = cloudDensityAt(q.xy * 0.001 + frame.weatherWind.zw, frame.weather.w, 3);
  return frame.lightningGlow.rgb * (spot * (0.3 + 0.7 * density) * exp(-t / 60000.0));
}

// Rain ringing standing water (a puddle, a river, a soaked flat stone): the
// slope (d height / d x and d y, world XY) of the ripples spreading from the
// drops landing at `p` (world XY, metres) `time` seconds in, as hard as
// `rain` (0..1). Three layers of cells 27-41 cm wide, each a drop landing
// somewhere in it every 0.6-0.8 s (more of them the harder it rains), its
// ring spreading to a quarter of the cell and dying away. Tilt an up-facing
// normal by it: normalize(n - vec3f(slope * strength, 0.0)) (strength about
// 1). 0 while it's dry. Puddles (and anything else holding water) share it.
fn rainRipples(p: vec2f, time: f32, rain: f32) -> vec2f {
  if (rain <= 0.0) {
    return vec2f(0.0);
  }
  var slope = vec2f(0.0);
  for (var layer = 0; layer < 3; layer++) {
    let size = 0.27 + 0.07 * f32(layer);
    let q = p / size + vec2f(f32(layer) * 7.31, f32(layer) * 3.17);
    let cell = vec2i(floor(q));
    let salt = vec2i(layer * 1013, layer * 271);
    // Each drop lands at its own time, then the next somewhere else.
    let period = 0.6 + 0.1 * f32(layer);
    let t = time / period + cloudHash(cell + salt);
    let drop = cell + salt + vec2i(i32(floor(t)) * 37, i32(floor(t)) * 11);
    if (cloudHash(drop + vec2i(5, 3)) > 0.15 + 0.85 * rain) {
      continue;
    }
    let age = fract(t);
    let center = vec2f(cell) + vec2f(0.25) + 0.5 * vec2f(cloudHash(drop + vec2i(37, 91)), cloudHash(drop + vec2i(113, 7)));
    let away = q - center;
    let d = length(away);
    // The ring (a wave train around radius r, in cells), fading as it goes.
    let width = 0.05;
    let x = (d - age * 0.25) / width;
    let wave = exp(-x * x) * (3.0 * cos(3.0 * x) - 2.0 * x * sin(3.0 * x));
    let fade = (1.0 - age) * (1.0 - age);
    // 1.5 mm high at first.
    slope += away / max(d, 1e-4) * (wave * fade * 0.0015 / (width * size));
  }
  return slope;
}

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
