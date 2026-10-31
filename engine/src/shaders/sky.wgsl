// A physical sky's reflections (after skyCommon.wgsl): fs_radiance renders
// the sky, seen from the ground, into an octahedral texture (the background
// and the lit pass's haze read it); fs_prefilter blurs that into each mip of
// the reflections the lit pass reads, sharp to rough (GGX, importance
// sampled). Each draws one full-screen triangle.
//
// The atmosphere is PhysicalSky's (sky.cpp), worked out the same way, so what
// the CPU's diffuse light and sun see is what's drawn: single scattering by
// air (Rayleigh) and haze (Mie) with ozone absorbing, an isotropic share for
// light scattered more than once, the ground's light below the horizon, and
// the night glow on top. Lengths in kilometres.

struct Sky {
  toSun: vec4f,    // xyz toward the sun (unit), w the solar irradiance
  rayleigh: vec4f, // rgb scattering per km at sea level, w scale height
  mie: vec4f,      // x scattering, y extinction (per km at sea level), z scale height, w g
  ozone: vec4f,    // rgb absorption per km at its densest
  ground: vec4f,   // rgb what the ground reflects
  night: vec4f,    // rgb the night glow at the zenith
  planet: vec4f,   // x ground radius, y top of the air, z the viewer's altitude, w multiple scattering's share
}

struct SkyPass {
  params: vec4f, // x roughness (fs_prefilter), y the source's size (texels), z samples
}

@group(0) @binding(0) var<uniform> sky: Sky;
// fs_radiance: the transmittance table; fs_prefilter: the sky.
@group(0) @binding(1) var source: texture_2d<f32>;
@group(0) @binding(2) var linearClamp: sampler;
@group(0) @binding(3) var<uniform> skyPass: SkyPass;

const PI: f32 = 3.14159265;
const VIEW_SAMPLES: i32 = 24;      // PhysicalSky::kViewSamples
const TABLE_ANGLES: f32 = 128.0;   // PhysicalSky::kTransmittanceAngles
const TABLE_HEIGHTS: f32 = 32.0;   // PhysicalSky::kTransmittanceHeights

struct Varyings {
  @builtin(position) position: vec4f,
  @location(0) uv: vec2f, // (0, 0) at the top left
}

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> Varyings {
  let corner = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
  var out: Varyings;
  out.position = vec4f(corner * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
  out.uv = corner;
  return out;
}

// How much light gets through from `altitude` along `cosZenith` to space
// (PhysicalSky::transmittance).
fn transmittance(altitude: f32, cosZenith: f32) -> vec3f {
  let top = sky.planet.y - sky.planet.x;
  let v = sqrt(clamp(altitude / top, 0.0, 1.0));
  let c = clamp(cosZenith, -1.0, 1.0);
  let u = 0.5 * (sign(c) * sqrt(abs(c)) + 1.0);
  let uv = vec2f((u * (TABLE_ANGLES - 1.0) + 0.5) / TABLE_ANGLES,
                 (v * (TABLE_HEIGHTS - 1.0) + 0.5) / TABLE_HEIGHTS);
  return textureSampleLevel(source, linearClamp, uv, 0.0).rgb;
}

fn ozoneDensity(altitude: f32) -> f32 {
  return max(0.0, 1.0 - abs(altitude - 25.0) / 15.0);
}

fn nightGlow(direction: vec3f) -> vec3f {
  let horizon = 1.0 - abs(clamp(direction.z, -1.0, 1.0));
  return sky.night.rgb * (1.0 + 1.5 * horizon * horizon * horizon);
}

// The sky's light from `direction`, seen from the viewer (no sun disk).
fn skyRadiance(direction: vec3f) -> vec3f {
  let ground = sky.planet.x;
  let top = sky.planet.y;
  let r0 = ground + sky.planet.z;
  let mu = direction.z;
  let toSun = sky.toSun.xyz;

  // Where the ray ends: the ground, or the top of the air.
  let groundDisc = r0 * r0 * (mu * mu - 1.0) + ground * ground;
  let hitsGround = mu < 0.0 && groundDisc >= 0.0;
  let topDisc = r0 * r0 * (mu * mu - 1.0) + top * top;
  var end = max(0.0, -r0 * mu + sqrt(max(topDisc, 0.0)));
  if (hitsGround) {
    end = max(0.0, -r0 * mu - sqrt(groundDisc));
  }

  let nu = dot(direction, toSun);
  let phaseR = 3.0 / (16.0 * PI) * (1.0 + nu * nu);
  let g = sky.mie.w;
  let g2 = g * g;
  let phaseM = 3.0 / (8.0 * PI) * (1.0 - g2) * (1.0 + nu * nu) /
               ((2.0 + g2) * pow(max(1.0 + g2 - 2.0 * g * nu, 1e-4), 1.5));

  var depth = vec3f(0.0);
  var light = vec3f(0.0);
  var previous = 0.0;
  for (var i = 0; i < VIEW_SAMPLES; i++) {
    // Denser near the viewer, where the air is.
    let f = f32(i + 1) / f32(VIEW_SAMPLES);
    let next = end * f * f;
    let dt = next - previous;
    let t = 0.5 * (previous + next);
    previous = next;

    let position = vec3f(0.0, 0.0, r0) + direction * t;
    let r = length(position);
    let h = max(r - ground, 0.0);
    let sunCos = dot(position / r, toSun);
    let densityR = exp(-h / sky.rayleigh.w);
    let densityM = exp(-h / sky.mie.z);
    let extinction = sky.rayleigh.rgb * densityR + vec3f(sky.mie.y * densityM) +
                     sky.ozone.rgb * ozoneDensity(h);

    let throughView = exp(-(depth + extinction * (0.5 * dt)));
    depth += extinction * dt;
    let throughSun = transmittance(h, sunCos);
    let scatterR = sky.rayleigh.rgb * densityR;
    let scatterM = vec3f(sky.mie.x * densityM);
    let single = scatterR * phaseR + scatterM * phaseM;
    let multiple = (scatterR + scatterM) * sky.planet.w;
    light += throughView * throughSun * (single + multiple) * dt;
  }
  light *= sky.toSun.w;
  if (hitsGround) {
    light += exp(-depth) * sky.ground.rgb;
  }
  return light + nightGlow(direction);
}

@fragment
fn fs_radiance(in: Varyings) -> @location(0) vec4f {
  return vec4f(skyRadiance(octDecode(in.uv)), 1.0);
}

// The i-th of n points spread evenly over the unit square (Hammersley).
fn hammersley(i: u32, n: u32) -> vec2f {
  var bits = i;
  bits = (bits << 16u) | (bits >> 16u);
  bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
  bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
  bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
  bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
  return vec2f(f32(i) / f32(n), f32(bits) * 2.3283064365386963e-10);
}

// The sky as a surface of `roughness` reflects it, toward each direction
// (the view taken as the normal's, as split-sum prefiltering does).
@fragment
fn fs_prefilter(in: Varyings) -> @location(0) vec4f {
  let normal = octDecode(in.uv);
  let roughness = skyPass.params.x;
  if (roughness < 0.01) {
    return vec4f(textureSampleLevel(source, linearClamp, octEncode(normal), 0.0).rgb, 1.0);
  }
  let a = roughness * roughness;
  let helper = select(vec3f(0.0, 0.0, 1.0), vec3f(1.0, 0.0, 0.0), abs(normal.z) > 0.999);
  let tangent = normalize(cross(helper, normal));
  let bitangent = cross(normal, tangent);
  let count = u32(skyPass.params.z);
  var sum = vec3f(0.0);
  var weight = 0.0;
  for (var i = 0u; i < count; i++) {
    let xi = hammersley(i, count);
    let phi = 2.0 * PI * xi.x;
    let cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    let sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    let halfway = tangent * (sinTheta * cos(phi)) + bitangent * (sinTheta * sin(phi)) + normal * cosTheta;
    let toLight = 2.0 * dot(normal, halfway) * halfway - normal;
    let ndotl = dot(normal, toLight);
    if (ndotl > 0.0) {
      sum += textureSampleLevel(source, linearClamp, octEncode(toLight), 0.0).rgb * ndotl;
      weight += ndotl;
    }
  }
  return vec4f(sum / max(weight, 1e-4), 1.0);
}
