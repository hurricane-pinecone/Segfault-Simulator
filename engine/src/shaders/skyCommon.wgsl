// Shared by the physical sky's passes (sky.wgsl), its background
// (skyBackground.wgsl) and the lit pass: directions <-> where they are in the
// sky's textures, which hold the whole sphere octahedrally (Z up: the upper
// half in the middle diamond, the lower folded into the corners).

// A unit direction's place in an octahedral texture (0..1).
fn octEncode(direction: vec3f) -> vec2f {
  let n = direction / (abs(direction.x) + abs(direction.y) + abs(direction.z));
  var p = n.xy;
  if (n.z < 0.0) {
    p = (vec2f(1.0) - abs(n.yx)) * select(vec2f(-1.0), vec2f(1.0), n.xy >= vec2f(0.0));
  }
  return p * 0.5 + vec2f(0.5);
}

// The direction a place in an octahedral texture holds (unit).
fn octDecode(uv: vec2f) -> vec3f {
  let f = uv * 2.0 - vec2f(1.0);
  var n = vec3f(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
  let t = max(-n.z, 0.0);
  n.x += select(t, -t, n.x >= 0.0);
  n.y += select(t, -t, n.y >= 0.0);
  return normalize(n);
}

// A physical sky's clouds (see SceneWeather, and PhysicalSky's cloudBase and
// cloudSides, which work their average light out the same way): a layer
// CLOUD_ALTITUDE up, its density a noise pattern drifting on the wind, more
// of it showing the cloudier it is: scattered cumulus at low cover, broken
// cloud, then a deck near 1. Lengths in kilometres.
struct CloudLayer {
  origin: vec2f,   // the viewer's place in the pattern (its world XY + the drift)
  cover: f32,      // 0..1
  darkness: f32,   // 0..1
  light: vec3f,    // on their tops: the sun's at their height, or the moon's
  base: vec3f,     // their undersides, lit through
  toLight: vec3f,  // toward that light (unit)
}

const CLOUD_ALTITUDE: f32 = 1.5; // PhysicalSky::kCloudAltitude
const CLOUD_CELL: f32 = 3.0;     // the pattern's largest features (km)
const CLOUD_PERIOD: i32 = 64;    // cells before it repeats (x CLOUD_CELL: kCloudPeriod)

fn cloudHash(cell: vec2i) -> f32 {
  var h = (u32(cell.x) * 0x8da6b343u) ^ (u32(cell.y) * 0xd8163841u);
  h = (h ^ (h >> 15u)) * 0x2c1b3c6du;
  h = (h ^ (h >> 12u)) * 0x297a2d39u;
  h = h ^ (h >> 15u);
  return f32(h >> 8u) * (1.0 / 16777215.0);
}

// Value noise (0..1), repeating every `period` cells.
fn cloudNoise(p: vec2f, period: i32) -> f32 {
  let cell = vec2i(floor(p));
  let f = fract(p);
  let u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
  let wrap = vec2i(period);
  let c0 = ((cell % wrap) + wrap) % wrap;
  let c1 = (((cell + vec2i(1)) % wrap) + wrap) % wrap;
  let a = cloudHash(c0);
  let b = cloudHash(vec2i(c1.x, c0.y));
  let c = cloudHash(vec2i(c0.x, c1.y));
  let d = cloudHash(c1);
  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Octaves of it, each twice as fine (and repeating as often over the
// world), averaged by their weight (0..1, mostly near 0.5).
fn cloudFbm(p: vec2f, octaves: i32) -> f32 {
  var sum = 0.0;
  var total = 0.0;
  var amplitude = 0.5;
  var q = p;
  var period = CLOUD_PERIOD;
  for (var i = 0; i < octaves; i++) {
    sum += cloudNoise(q, period) * amplitude;
    total += amplitude;
    q = q * 2.0 + vec2f(17.3, 9.1);
    period *= 2;
    amplitude *= 0.5;
  }
  return sum / total;
}

// How dense the layer is at `p` (km in its pattern), 0..1: the noise's
// peaks at low cover (soft-edged puffs), most of it at higher, and a whole
// deck (textured a little) from about 0.7 up.
fn cloudDensityAt(p: vec2f, cover: f32, octaves: i32) -> f32 {
  let n = cloudFbm(p / CLOUD_CELL, octaves);
  let threshold = 0.5 + 0.24 * (0.5 - cover);
  let density = smoothstep(threshold, threshold + mix(0.05, 0.16, cover), n);
  let deck = smoothstep(0.7, 1.0, cover);
  return max(density, deck * clamp(0.75 + 2.0 * (n - 0.5), 0.0, 1.0));
}

// The layer's optical thickness (PhysicalSky's cloudThickness).
fn cloudThickness(cover: f32, darkness: f32) -> f32 {
  return (2.0 + 7.0 * cover) * (1.0 + 2.0 * darkness);
}

// How much of the light broken clouds' sides catch (PhysicalSky::cloudSides).
fn cloudSides(cover: f32, lightUp: f32) -> f32 {
  let low = 1.0 - smoothstep(0.0, 0.35, lightUp);
  return (1.0 - 0.8 * clamp(cover, 0.0, 1.0)) * (0.25 + 0.5 * low);
}

// The clear sky between clouds, paler the cloudier it is (PhysicalSky's
// cloudySky).
fn cloudySky(sky: vec3f, cover: f32) -> vec3f {
  let grey = dot(sky, vec3f(0.2126, 0.7152, 0.0722));
  return mix(sky, vec3f(grey), 0.6 * cover);
}

// The clouds' light on average: their undersides, and half of what their
// sides catch.
fn cloudAverage(layer: CloudLayer) -> vec3f {
  return layer.base + layer.light * (0.5 * cloudSides(layer.cover, layer.toLight.z));
}

// The clouds toward `direction` (unit; seen from the ground): rgb their
// light, a how much of what's behind them they hide (fading out far off,
// into the haze toward the horizon). None below the horizon.
fn cloudsToward(direction: vec3f, layer: CloudLayer) -> vec4f {
  if (layer.cover <= 0.0 || direction.z <= 0.0) {
    return vec4f(0.0);
  }
  // Where the view meets the layer, round the planet's curve.
  let r0 = 6360.1;
  let rc = 6360.0 + CLOUD_ALTITUDE;
  let b = r0 * direction.z;
  let c = (rc - r0) * (rc + r0);
  let t = c / (b + sqrt(b * b + c));
  let p = layer.origin + direction.xy * t;
  let density = cloudDensityAt(p, layer.cover, 6);
  if (density <= 0.002) {
    return vec4f(0.0);
  }
  let thickness = cloudThickness(layer.cover, layer.darkness);

  // How much of the light reaches the part seen: a few steps through the
  // layer toward it (further sideways, the lower it is).
  let toLight = layer.toLight;
  let across = toLight.xy / max(length(toLight.xy), 1e-4);
  let slope = min(length(toLight.xy) / max(toLight.z, 0.05), 6.0);
  let stride = 0.2 + 0.12 * slope;
  var depth = density * 0.5;
  for (var i = 1; i <= 3; i++) {
    depth += cloudDensityAt(p + across * (stride * f32(i)), layer.cover, 3);
  }
  let reach = exp(-depth * thickness * 0.12);
  // Thin edges toward the light glow (a silver lining).
  let toward = max(dot(direction, toLight), 0.0);
  let silver = 1.0 + 8.0 * pow(toward, 6.0) * (1.0 - 0.7 * density);
  let color = layer.base * mix(1.2, 0.85, density) +
              layer.light * (cloudSides(layer.cover, toLight.z) * reach * silver);

  // Thicker seen through a slant; faded into the haze far off.
  let hidden = 1.0 - exp(-density * (1.5 + 0.4 * thickness) / max(direction.z, 0.15));
  let fade = exp(-t / 60.0);
  return vec4f(color, hidden * fade);
}
