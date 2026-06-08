#define MAX_LIGHTS 128

in vec2 vWorldPosition;
in vec4 vColor;
in vec2 vUv;
in vec4 vParams;
in vec2 vShoreDir; // toward-shore direction (for the per-pixel wave normal)
in float vShoreInf; // how strongly the shore steers the waves here
in float vFade;    // wave amplitude scale

out vec4 FragColor;

uniform float uTime;
uniform float uRippleStrength;
uniform float uAmbient;

// Cutaway: drop surface water above the clip level / outside the radius, so it
// doesn't hang over the player when they're in a cave (matches the terrain cut).
uniform float uClipElevation;
uniform vec2 uClipCenter;
uniform float uClipRadius;

uniform int uLightCount;
uniform vec2 uLightPositions[MAX_LIGHTS];
uniform vec3 uLightColors[MAX_LIGHTS];
uniform float uLightIntensities[MAX_LIGHTS];
uniform float uLightRadii[MAX_LIGHTS];

// Sun + heightmap, for casting terrain shadows onto the water (the same horizon
// march the terrain geometry shader uses, so water and land share one shadow).
uniform vec3 uLightDirection;
uniform float uDiffuseStrength;
uniform int uSunShadowEnabled;
uniform sampler2D uHeightmap;
uniform vec2 uHeightmapOrigin;
uniform vec2 uHeightmapSize;
uniform float uHeightmapTexSize;

float hash21(vec2 p)
{
  p = fract(p * vec2(123.34, 456.21));
  p += dot(p, p + 45.32);
  return fract(p.x * p.y);
}

vec2 hash22(vec2 p)
{
  return vec2(
      hash21(p),
      hash21(p + 19.19));
}

float causticCells(vec2 p)
{
  vec2 i = floor(p);
  vec2 f = fract(p);

  float d1 = 10.0;
  float d2 = 10.0;

  for (int y = -1; y <= 1; y++)
  {
    for (int x = -1; x <= 1; x++)
    {
      vec2 g = vec2(float(x), float(y));
      vec2 o = hash22(i + g);

      o = 0.5 + 0.5 * sin(uTime * 0.75 + 6.2831 * o);

      vec2 r = g + o - f;
      float d = dot(r, r);

      if (d < d1)
      {
        d2 = d1;
        d1 = d;
      }
      else if (d < d2)
      {
        d2 = d;
      }
    }
  }

  float edge = sqrt(d2) - sqrt(d1);

  return 1.0 - smoothstep(0.035, 0.105, edge);
}

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

// Terrain (sun) shadow at a water point: 1 = lit, 0 = fully shadowed. Horizon
// march toward the sun against the heightmap -- identical to the terrain
// shader, so a cliff/mountain shadow falls across land and water coherently.
float sunVisibility(vec2 fromWorld, float fragGround)
{
  if (uHeightmapSize.x < 1.0)
    return 1.0;

  vec2 dir = uLightDirection.xy;
  float horiz = length(dir);
  if (horiz < 1.0e-4)
    return 1.0;

  dir /= horiz;
  float sunSlope = uLightDirection.z / horiz;
  float maxAngle = -1.0e9;

  // Always the SMOOTH march for water (even when land uses the sharp per-tile
  // shadow): fixed steps against the linearly-filtered heightmap give a soft,
  // continuous shadow instead of blocky tile-aligned edges on the surface.
  const int STEPS = 24;
  const float kSunShadowMaxDist = 16.0;
  for (int s = 0; s < STEPS; s++)
  {
    float d = kSunShadowMaxDist * (float(s) + 1.0) / float(STEPS);
    if (d < 0.5)
      continue;
    float th = terrainLevelAt(fromWorld + dir * d);
    if (th < -1.0e8)
      continue;
    maxAngle = max(maxAngle, (th - fragGround) / d);
  }

  // Wide penumbra for a soft shoreline shadow.
  return 1.0 - smoothstep(0.0, 0.40, maxAngle - sunSlope);
}

float waveWeight(vec2 dir, vec2 shoreDir, float inf)
{
  float align = dot(normalize(dir), shoreDir);
  return mix(1.0, clamp(align, 0.0, 1.0) * 1.3, inf);
}

// Per-pixel wave normal: the height gradient is rebuilt here (not interpolated
// from the coarse vertices), so the lighting stays smooth no matter how the
// surface is tessellated. The wave SET (directions, amplitude weights) must
// match surface.vert's gerstner calls.
vec2 waveGradient(vec2 p)
{
  vec2 D0 = vec2(1.0, 0.4), D1 = vec2(-0.6, 1.0);
  vec2 D2 = vec2(0.3, -1.0), D3 = vec2(-1.0, -0.3);

  vec2 g = vec2(0.0);
  vec2 d;
  float k;
  d = normalize(D0);
  k = 6.2831853 / 8.0;
  g += waveWeight(D0, vShoreDir, vShoreInf) * 0.20 * k *
       cos(k * dot(d, p) - 2.0 * uTime) * d;
  d = normalize(D1);
  k = 6.2831853 / 6.0;
  g += waveWeight(D1, vShoreDir, vShoreInf) * 0.17 * k *
       cos(k * dot(d, p) - 1.7 * uTime) * d;
  d = normalize(D2);
  k = 6.2831853 / 4.0;
  g += waveWeight(D2, vShoreDir, vShoreInf) * 0.13 * k *
       cos(k * dot(d, p) - 1.4 * uTime) * d;
  d = normalize(D3);
  k = 6.2831853 / 2.6;
  g += waveWeight(D3, vShoreDir, vShoreInf) * 0.09 * k *
       cos(k * dot(d, p) - 1.2 * uTime) * d;
  return g * vFade;
}

// Extra high-frequency choppy detail for the NORMAL only (not the geometry, so
// it can't re-facet the surface). Directions step by the golden angle so they
// scatter instead of forming a regular lattice -- this breaks up the rectangular
// look of the few main swells in open water.
vec2 detailGradient(vec2 p)
{
  vec2 g = vec2(0.0);
  float ang = 0.7;
  float L = 5.5;
  float speed = 1.3;
  for (int i = 0; i < 4; i++)
  {
    ang += 2.39996; // golden angle (radians) -> scattered, non-lattice
    L *= 0.82;      // medium wavelengths -> dispersion, not chop
    speed += 0.25;
    vec2 d = vec2(cos(ang), sin(ang));
    float k = 6.2831853 / L;
    g += 0.009 * k * cos(k * dot(d, p) - speed * uTime) * d;
  }
  return g;
}

void main()
{
  // Cutaway: hide water above the player's roof / outside the cave bubble.
  if (vParams.z > uClipElevation)
    discard;
  if (uClipRadius > 0.0 && distance(vWorldPosition, uClipCenter) > uClipRadius)
    discard;

  float r1 = sin(
      vWorldPosition.x * 1.25 +
      vWorldPosition.y * 0.55 +
      uTime * 2.2);

  float r2 = sin(
      vWorldPosition.x * -0.75 +
      vWorldPosition.y * 1.10 +
      uTime * 1.6);

  float ripple = (r1 + r2) * 0.5;

  vec3 color = vColor.rgb;
  color += ripple * uRippleStrength;

  // Surface normal from the per-pixel wave gradient (z up). Lighting against
  // this is what makes highlights ride the moving crests, and computing it per
  // pixel keeps it smooth regardless of the surface tessellation. The scattered
  // detail adds choppy dispersion to open water (faded out toward the shore).
  vec2 waveGrad =
      waveGradient(vWorldPosition) + detailGradient(vWorldPosition) * vFade;
  vec3 N = normalize(vec3(-waveGrad, 1.0));
  const vec3 V = vec3(0.0, 0.0, 1.0); // iso view ~ straight down

  vec3 pointLight = vec3(0.0);
  vec3 specular = vec3(0.0);
  const float kLightHeight = 4.0; // lamps sit this far above the water

  for (int i = 0; i < MAX_LIGHTS; i++)
  {
    if (i >= uLightCount)
      break;

    vec2 toLight = uLightPositions[i] - vWorldPosition;
    float dist = length(toLight);

    if (dist >= uLightRadii[i])
      continue;

    float attenuation = 1.0 - dist / uLightRadii[i];
    attenuation = clamp(attenuation, 0.0, 1.0);
    attenuation = pow(attenuation, 1.35);

    vec3 L = normalize(vec3(toLight, kLightHeight));
    float ndl = max(dot(N, L), 0.0);

    // Diffuse follows the wave normal, so the lit band rides the crests.
    pointLight += uLightColors[i] * uLightIntensities[i] * attenuation * 0.45 *
                  (0.4 + 0.6 * ndl);

    // Sharp glints on the wave flanks facing the light.
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), 48.0);
    specular += uLightColors[i] * uLightIntensities[i] * attenuation * spec * 1.1;
  }

  float depth = vParams.x;

  vec2 p = vWorldPosition * 1.75;

  float c1 = causticCells(p + vec2(uTime * 0.08, uTime * 0.04));
  float c2 = causticCells(p * 1.7 - vec2(uTime * 0.05, uTime * 0.09));

  float caustic = max(c1, c2 * 0.25);
  caustic = pow(caustic, 2.4);

  float shallowFactor =
      1.0 - smoothstep(0.25, 3.0, depth);

  shallowFactor = pow(shallowFactor, 1.8);

  // Caustics are the sun refracting onto the shallow floor -- driven only by
  // shallow depth + daylight, NOT by point lights (a lamp lights the water
  // surface/crests, not this floor refraction).
  float causticVisibility =
      smoothstep(0.18, 0.75, uAmbient);

  float causticStrength =
      0.25 * shallowFactor;

  vec3 causticColor = vec3(0.85, 0.97, 1.0);

  // The actual sun direction (toward the sun) -- the same vector the shadow
  // march uses, not a fake one. Lighting against it makes the daylight
  // highlights track the real sun instead of looking baked-in.
  vec3 sunDir = length(uLightDirection) > 1.0e-4 ? normalize(uLightDirection)
                                                 : vec3(0.0, 0.0, 1.0);
  float sunNdl = max(dot(N, sunDir), 0.0);

  // Terrain casts its sun shadow onto the water (heightmap march at the water's
  // surface level). Only the SUN-driven terms darken -- point lights (lamps)
  // still light shadowed water.
  float sunShadow = uSunShadowEnabled == 1
                        ? sunVisibility(vWorldPosition, vParams.z)
                        : 1.0;
  float sunLit = mix(1.0 - uDiffuseStrength, 1.0, sunShadow);
  // The sun shades the waves by their facing (like the lamps do at night) over
  // an ambient fill, then darkens where it's in shadow.
  float sunAmbient = uAmbient * sunLit * mix(0.65, 1.0, sunNdl);

  float lightFloor = mix(0.06, 0.82, clamp(uAmbient, 0.0, 1.0));
  color *= max(sunAmbient, lightFloor);

  // Keep transparent water readable at night.
  vec3 nightWaterFloor = vColor.rgb * 0.22;
  color = max(color, nightWaterFloor);

  color +=
      causticColor *
      caustic *
      causticStrength *
      causticVisibility *
      sunShadow;

  color += pointLight;

  // Sun sparkle on the crests: a tight Blinn-Phong glint about the REAL sun
  // direction, so it sits where the sun actually reflects toward the view and
  // shifts as the sun moves. It eases off as the sun climbs -- an overhead sun
  // reflects straight up into this top-down view, which would otherwise sheen
  // the whole surface at midday.
  vec3 sunHalf = normalize(sunDir + V);
  float sunSpec = pow(max(dot(N, sunHalf), 0.0), 90.0);
  float glintFalloff = 1.0 - 0.75 * clamp(sunDir.z, 0.0, 1.0);
  // A cool, pale tint (not pure white) so the glint blends with the water.
  color += vec3(0.62, 0.78, 0.95) * sunSpec * clamp(uAmbient, 0.0, 1.0) * 0.3 *
           sunShadow * glintFalloff;

  // Point-light glints (added after diffuse so crests catch the lamps).
  color += specular;

  color = clamp(color, 0.0, 1.0);

  FragColor = vec4(color, vColor.a);
}
