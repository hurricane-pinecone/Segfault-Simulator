// A physical sky's background, in the lit pass after the opaque things (after
// meshCommon.wgsl and skyCommon.wgsl): one full-screen triangle at the far
// plane, so it only fills what nothing covered. The sky as sky.wgsl's
// fs_radiance rendered it, the sun's and the moon's disks, and stars at night; scaled by a
// visual style's sky balance; into height fog toward the horizon. It writes
// the light that blooms too (all of it: the sun's disk is what's bright
// enough).

@group(0) @binding(8) var skySampler: sampler;
@group(0) @binding(9) var skyRadiance: texture_2d<f32>;

struct BackgroundOut {
  @builtin(position) position: vec4f,
  @location(0) ndc: vec2f,
}

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> BackgroundOut {
  let corner = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
  let ndc = corner * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0);
  var out: BackgroundOut;
  out.position = vec4f(ndc, 1.0, 1.0);
  out.ndc = ndc;
  return out;
}

struct SkyOut {
  @location(0) color: vec4f,
  @location(1) glow: vec4f,
}

// A hash of a cell, 0..1.
fn cellHash(cell: vec2f) -> f32 {
  let p = fract(cell * vec2f(0.1031, 0.1030));
  let q = p + dot(p, p.yx + 33.33);
  return fract((q.x + q.y) * q.x);
}

@fragment
fn fs_main(in: BackgroundOut) -> SkyOut {
  // The view ray through this pixel.
  let near = frame.inverseViewProjection * vec4f(in.ndc, 0.0, 1.0);
  let far = frame.inverseViewProjection * vec4f(in.ndc, 1.0, 1.0);
  let direction = normalize(far.xyz / far.w - near.xyz / near.w);
  let toSun = frame.sunDirection.xyz;

  // How far from the sun's centre, in its radii (derivatives first, while
  // control flow is uniform).
  let radius = max(frame.sunLight.w, 1e-5);
  let fromSun = length(cross(direction, toSun)) / radius;
  let edge = max(fwidth(fromSun), 1e-4);
  // And from the moon's, in its radii.
  let toMoon = frame.moon.xyz;
  let moonRadius = max(frame.moon.w, 1e-5);
  let moonOffset = direction - toMoon * dot(direction, toMoon);
  let fromMoon = length(moonOffset) / moonRadius;
  let moonEdge = max(fwidth(fromMoon), 1e-4);

  // The sky: above the horizon as rendered; below it, the horizon's haze
  // giving way to the ground's light further down (there's no ground drawn
  // there: only the world's edge shows it). The horizon is kept a texel clear
  // of the seam between the halves in the sky's texture.
  let level = normalize(vec3f(direction.xy, max(direction.z, 0.012)));
  var color = textureSampleLevel(skyRadiance, skySampler, octEncode(level), 0.0).rgb;
  if (direction.z < 0.0) {
    let ground = textureSampleLevel(skyRadiance, skySampler, octEncode(direction), 0.0).rgb;
    color = mix(color, ground, smoothstep(0.0, 0.45, -direction.z));
  }

  // The disk, darker toward its edge (limb darkening), above the horizon;
  // unless the frame's light is the moon's (then toSun is toward it).
  if (frame.moonLight.w < 0.5 && dot(direction, toSun) > 0.0 && fromSun < 1.0 + edge) {
    let mu = sqrt(max(1.0 - fromSun * fromSun, 0.0));
    let limb = 1.0 - 0.6 * (1.0 - mu);
    let coverage = 1.0 - smoothstep(1.0 - edge, 1.0 + edge, fromSun);
    // (The sun's light is already adapted, as the sky is below: once only;
    // and faded near the horizon, which its disk isn't.)
    color += frame.sunLight.rgb *
             (frame.skyScale.w * limb * coverage / (max(frame.skyLight.x, 1e-4) * max(frame.skyLight.y, 1e-4)));
  }

  // The moon: lit on the sun's side, as much of it as its phase says (the
  // line between an ellipse across it), its dark side faintly (earthshine).
  if (frame.moon.w > 0.0 && direction.z > 0.0 && dot(direction, toMoon) > 0.0 && fromMoon < 1.0 + moonEdge) {
    var sunward = frame.moonSun.xyz - toMoon * dot(frame.moonSun.xyz, toMoon);
    if (dot(sunward, sunward) < 1e-8) {
      sunward = cross(toMoon, vec3f(0.0, 0.0, 1.0)) + vec3f(1e-4, 0.0, 0.0);
    }
    sunward = normalize(sunward);
    let u = dot(moonOffset, sunward) / moonRadius;
    let v = dot(moonOffset, cross(toMoon, sunward)) / moonRadius;
    let across = sqrt(max(1.0 - v * v, 0.0));
    let terminator = (1.0 - 2.0 * frame.moonSun.w) * across;
    let lit = smoothstep(terminator - 0.06, terminator + 0.06, u);
    let mu = sqrt(max(1.0 - fromMoon * fromMoon, 0.0));
    let limb = 1.0 - 0.3 * (1.0 - mu);
    let coverage = 1.0 - smoothstep(1.0 - moonEdge, 1.0 + moonEdge, fromMoon);
    // (Not adapted yet, unlike the sun's: the sky's scale below does it.)
    let moonDisk = frame.moonLight.rgb * (mix(0.015, 1.0, lit) * limb);
    color += moonDisk * (coverage * smoothstep(0.0, 0.01, direction.z));
  }

  // Stars, where the sky's dark enough to see them.
  let brightness = dot(color, vec3f(0.2126, 0.7152, 0.0722));
  let starry = frame.skyMode.w * (1.0 - smoothstep(0.004, 0.03, brightness));
  if (starry > 0.0 && direction.z > 0.0) {
    let cells = octEncode(direction) * 1400.0;
    let cell = floor(cells);
    let pick = cellHash(cell);
    if (pick > 0.996) {
      let spot = length(fract(cells) - vec2f(0.5)) * 2.0;
      let twinkle = cellHash(cell + vec2f(17.0, 5.0));
      let star = (1.0 - smoothstep(0.0, 1.0, spot)) * mix(0.01, 0.12, twinkle * twinkle);
      color += vec3f(0.85, 0.9, 1.0) * star * starry * smoothstep(0.0, 0.08, direction.z);
    }
  }

  color *= frame.skyScale.rgb;

  // Height fog (fog.wgsl), as far off as the far plane: toward the horizon,
  // the fog's colour (the sky's there, glowing toward the sun).
  if (frame.fogLight.z > 0.5) {
    let fogged = fogAt(frame.cameraPosition.xyz + direction * frame.fogLight.w);
    let horizon = normalize(vec3f(direction.xy, max(direction.z, 0.0) + 0.05 * length(direction.xy) + 1e-4));
    let fog = textureSampleLevel(skyRadiance, skySampler, octEncode(horizon), 0.0).rgb * frame.skyScale.rgb +
              fogSunGlow(direction);
    color = mix(color, fog, fogged);
  }
  return SkyOut(vec4f(color, 1.0), vec4f(color, 1.0));
}
