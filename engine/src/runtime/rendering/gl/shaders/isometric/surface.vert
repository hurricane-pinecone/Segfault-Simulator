layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aWorldPosition;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec2 aUv;
layout(location = 4) in vec4 aParams;
layout(location = 5) in float aZ;

out vec2 vWorldPosition;
out vec4 vColor;
out vec2 vUv;
out vec4 vParams;
out vec2 vShoreDir; // toward-shore direction, for the per-pixel wave normal
out float vShoreInf; // how strongly the shore steers the waves here (0 = open)
out float vFade;     // wave amplitude scale (shore fade * uWaveStrength)

uniform float uTime;        // shared with the fragment stage
uniform float uWaveStrength; // 0 = flat plane (no Gerstner displacement)

// Projection's linear basis: clip-space delta per unit of world x / y /
// elevation. Lets us displace the surface in WORLD space (real Gerstner waves)
// and re-project, instead of a flat screen-space bob.
uniform vec2 uWorldToClipX;
uniform vec2 uWorldToClipY;
uniform vec2 uWorldToClipE;

// The heightmap (same one the fragment stage uses for shadows) -- here it gives
// the seabed slope, so the waves heading toward shore can be favoured.
uniform sampler2D uHeightmap;
uniform vec2 uHeightmapOrigin;
uniform vec2 uHeightmapSize;
uniform float uHeightmapTexSize;

// The fixed wave directions (kept in sync with surface.frag). They are NOT bent
// per point -- bending warps the wave phase and the surface tessellates. Instead
// each is weighted by how much it heads toward shore (waveWeight).
const vec2 kDir0 = vec2(1.0, 0.4);
const vec2 kDir1 = vec2(-0.6, 1.0);
const vec2 kDir2 = vec2(0.3, -1.0);
const vec2 kDir3 = vec2(-1.0, -0.3);

float terrainLevelAt(vec2 world)
{
  if (uHeightmapSize.x < 1.0 || uHeightmapSize.y < 1.0)
    return -1e9;
  vec2 texel = world - uHeightmapOrigin;
  if (texel.x < 0.0 || texel.x >= uHeightmapSize.x || texel.y < 0.0 ||
      texel.y >= uHeightmapSize.y)
    return -1e9;
  return texture(uHeightmap, texel / uHeightmapTexSize).r;
}

// Direction up the seabed slope (toward the shore) + how strongly it applies.
vec2 shoreDirection(vec2 world, out float strength)
{
  float s = 4.0; // wide baseline -> the overall shore direction, not a local edge
  float hl = terrainLevelAt(world - vec2(s, 0.0));
  float hr = terrainLevelAt(world + vec2(s, 0.0));
  float hd = terrainLevelAt(world - vec2(0.0, s));
  float hu = terrainLevelAt(world + vec2(0.0, s));
  if (hl < -1e8 || hr < -1e8 || hd < -1e8 || hu < -1e8)
  {
    strength = 0.0;
    return vec2(1.0, 0.0);
  }

  vec2 grad = vec2(hr - hl, hu - hd); // uphill = toward shore
  float mag = length(grad);
  strength = clamp(mag * 0.4, 0.0, 1.0);
  return mag > 1.0e-4 ? grad / mag : vec2(1.0, 0.0);
}

// Amplitude weight for a wave: in open water all waves are full; toward a shore
// the waves heading at it are favoured (and slightly boosted, so they build as
// they roll in) while those heading away fade out.
float waveWeight(vec2 dir, vec2 shoreDir, float inf)
{
  float align = dot(normalize(dir), shoreDir);
  return mix(1.0, clamp(align, 0.0, 1.0) * 1.3, inf);
}

// One Gerstner wave (fixed direction): crests pinch toward sharp peaks while the
// surface rises/falls. Returns the (worldX, worldY, elevation) offset.
vec3 gerstner(vec2 p, vec2 dir, float wavelength, float amp, float steep,
              float speed)
{
  vec2 d = normalize(dir);
  float k = 6.2831853 / wavelength;
  float phase = k * dot(d, p) - speed * uTime;
  return vec3(d * (steep * amp * cos(phase)), amp * sin(phase));
}

void main()
{
  vWorldPosition = aWorldPosition;
  vColor = aColor;
  vUv = aUv;
  vParams = aParams;

  vec2 p = aWorldPosition;

  // How strongly the shore steers the waves: only in shallow water on a slope.
  float shoreStrength;
  vec2 shoreDir = shoreDirection(p, shoreStrength);
  float inf = shoreStrength * (1.0 - smoothstep(0.0, 8.0, aParams.x));

  // Balanced crossing trains (long to short). KEEP IN SYNC with waveGradient()
  // in surface.frag. Directions are fixed; only the amplitude weight changes.
  vec3 disp = vec3(0.0);
  disp += waveWeight(kDir0, shoreDir, inf) * gerstner(p, kDir0, 8.0, 0.20, 0.70, 2.0);
  disp += waveWeight(kDir1, shoreDir, inf) * gerstner(p, kDir1, 6.0, 0.17, 0.75, 1.7);
  disp += waveWeight(kDir2, shoreDir, inf) * gerstner(p, kDir2, 4.0, 0.13, 0.80, 1.4);
  disp += waveWeight(kDir3, shoreDir, inf) * gerstner(p, kDir3, 2.6, 0.09, 0.85, 1.2);

  // Calm the water as it shallows toward the shore (aParams.x = depth in
  // levels). A gentle power softens the ramp so the immediate edge is calmest.
  // uWaveStrength scales the whole effect: 0 leaves a flat plane (ECS water).
  float fade = pow(smoothstep(0.0, 3.5, aParams.x), 1.4) * uWaveStrength;
  disp *= fade;

  // Project the world-space displacement into clip space and apply it.
  vec2 clipOffset = disp.x * uWorldToClipX + disp.y * uWorldToClipY +
                    disp.z * uWorldToClipE;

  vShoreDir = shoreDir;
  vShoreInf = inf;
  vFade = fade;

  gl_Position = vec4(aPosition + clipOffset, aZ, 1.0);
}
