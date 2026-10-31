// Height fog (FogSettings), for the lit passes and the sky's background
// (after meshCommon.wgsl). Must match fogOpacity() and fogPhase() in fog.cpp.

// How much of a point height fog hides, 0 to its most: its density
// (thickest at its base height, thinning exponentially above) along the line
// from the camera, past where it starts, in closed form.
fn fogAt(worldPosition: vec3f) -> f32 {
  let camera = frame.cameraPosition.xyz;
  let away = worldPosition - camera;
  let reach = length(away);
  let start = frame.fog.w;
  if (reach <= start || reach <= 0.0) {
    return 0.0;
  }
  let travelled = reach - start;
  let startHeight = camera.z + away.z * (start / reach);
  let rise = away.z * (travelled / reach);
  let k = frame.fog.y;
  let near = exp(min(-k * (startHeight - frame.fog.z), 80.0));
  let far = exp(min(-k * (startHeight + rise - frame.fog.z), 80.0));
  let slope = k * rise;
  let level = abs(slope) <= 1e-3;
  let mean = select((near - far) / select(slope, 1.0, level), 0.5 * (near + far), level);
  let depth = frame.fog.x * travelled * mean;
  return min(1.0 - exp(-depth), frame.fogLight.x);
}

// The sun's light the fog scatters toward the eye looking along `direction`
// (unit): Henyey-Greenstein's phase, x the fog's glow. Only under a physical
// sky (whose sun's light it is).
fn fogSunGlow(direction: vec3f) -> vec3f {
  let g = 0.75;
  let c = clamp(dot(direction, frame.sunDirection.xyz), -1.0, 1.0);
  let d = 1.0 + g * g - 2.0 * g * c;
  let phase = (1.0 - g * g) / (4.0 * 3.14159265 * d * sqrt(d));
  return frame.sunLight.rgb * (phase * frame.fogLight.y);
}
