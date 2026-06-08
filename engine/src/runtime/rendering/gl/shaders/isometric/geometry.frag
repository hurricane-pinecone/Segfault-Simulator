#define MAX_LIGHTS 128

in vec2 vUv;
in vec2 vWorldPos;
in float vGround;
in vec3 vNormal;

out vec4 FragColor;

uniform sampler2D uTexture;
uniform float uAmbient;
uniform vec3 uLightDirection;
uniform vec3 uLightColor; // sun/ambient scene tint
uniform float uDiffuseStrength;
uniform int uSurfaceEffect;
uniform int uShadowSharp;      // 0 = smooth (bilinear), 1 = sharp (per-tile DDA)
uniform int uSunShadowEnabled; // 1 = cast terrain shadows via the heightmap march

uniform int uLightCount;
uniform vec2 uLightPositions[MAX_LIGHTS];
uniform vec3 uLightColors[MAX_LIGHTS];
uniform float uLightIntensities[MAX_LIGHTS];
uniform float uLightRadii[MAX_LIGHTS];
uniform float uLightHeights[MAX_LIGHTS];
uniform float uLightGroundLevels[MAX_LIGHTS];

uniform sampler2D uHeightmap;
uniform vec2 uHeightmapOrigin;
uniform vec2 uHeightmapSize;
uniform float uHeightmapTexSize;
uniform float uHeightScale;

// Cutaway: when the player is in a cave, terrain above uClipElevation (the roof)
// and outside uClipRadius of uClipCenter (the surrounding rock + far world) is
// dropped, so only the cave around the player shows. uClipRadius <= 0 disables
// the localization; a large uClipElevation disables the roof cut.
uniform float uClipElevation;
uniform vec2 uClipCenter;
uniform float uClipRadius;

float hash21(vec2 p)
{
  p = fract(p * vec2(123.34, 456.21));
  p += dot(p, p + 45.32);
  return fract(p.x * p.y);
}

float valueNoise(vec2 p)
{
  vec2 i = floor(p);
  vec2 f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  float a = hash21(i);
  float b = hash21(i + vec2(1.0, 0.0));
  float c = hash21(i + vec2(0.0, 1.0));
  float d = hash21(i + vec2(1.0, 1.0));
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

vec3 applyGrassEffect(vec3 color, vec2 uv, vec2 worldPos)
{
  vec2 p = worldPos * 12.0 + uv * 28.0;
  float n1 = valueNoise(p);
  float n2 = valueNoise(p * 2.7);
  float variation = n1 * 0.65 + n2 * 0.35;
  vec3 dark = vec3(0.55, 0.85, 0.45);
  vec3 light = vec3(1.15, 1.28, 0.82);
  color *= mix(dark, light, variation);
  float blade = smoothstep(0.72, 0.92, valueNoise(vec2(p.x * 0.7, p.y * 3.5)));
  color *= mix(vec3(1.0), vec3(0.75, 1.12, 0.65), blade * 0.35);
  return color;
}

vec3 applySandEffect(vec3 color, vec2 uv, vec2 worldPos)
{
  vec2 p = worldPos * 18.0 + uv * 72.0;
  float along = p.x * 0.85 + p.y * 0.32;
  float across = p.x * -0.28 + p.y * 0.96;
  float warp = valueNoise(vec2(along * 0.08, across * 0.18)) * 2.0 - 1.0;
  float dune = sin(along * 0.42 + warp * 2.8) * 0.5 + 0.5;
  dune = smoothstep(0.38, 0.72, dune);
  float fineDune = sin(along * 1.4 + warp * 3.5) * 0.5 + 0.5;
  fineDune = smoothstep(0.48, 0.82, fineDune);
  float grain = valueNoise(p * 3.7);
  float specks = smoothstep(0.90, 0.985, valueNoise(p * 10.0));
  vec3 sand = color;
  sand *= mix(0.90, 1.10, dune * 0.25);
  sand *= mix(0.96, 1.06, fineDune * 0.15);
  sand *= mix(0.97, 1.03, grain * 0.5);
  sand -= specks * 0.025;
  return sand;
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

float pointLightVisibility(vec2 fragXY, vec2 lightXY, float fragGround,
                           float lightLevel)
{
  if (uHeightmapSize.x < 1.0)
    return 1.0;
  float anchor = fragGround < -1.0e8 ? lightLevel : fragGround;
  vec2 toLight = lightXY - fragXY;
  float distTotal = length(toLight);
  if (distTotal < 1.0e-4)
    return 1.0;
  float lightAngle = (lightLevel - anchor) / distTotal;
  const int STEPS = 48;
  float maxAngle = -1.0e9;
  for (int s = 0; s < STEPS; s++)
  {
    float t = (float(s) + 0.5) / float(STEPS);
    float d = distTotal * t;
    if (d < 0.85)
      continue;
    vec2 samplePos = mix(fragXY, lightXY, t);
    float terrainAngle = (terrainLevelAt(samplePos) - anchor) / d;
    maxAngle = max(maxAngle, terrainAngle);
  }
  float over = maxAngle - lightAngle;
  return 1.0 - smoothstep(0.0, 0.30, over);
}

// Real-normal point lighting. fragGround is the fragment's own elevation (vGround),
// so a side face's lower fragments are close to a low light and light up.
vec3 calculatePointLighting(vec3 normal)
{
  vec3 weightedColor = vec3(0.0);
  float totalWeight = 0.0;
  float strongestAmount = 0.0;

  float fragGroundSmooth = vGround;

  for (int i = 0; i < MAX_LIGHTS; i++)
  {
    if (i >= uLightCount)
      break;

    vec2 delta = uLightPositions[i] - vWorldPos;
    float distXY = length(delta);
    if (distXY >= uLightRadii[i])
      continue;

    float lightLevel = uLightGroundLevels[i] + uLightHeights[i] * uHeightScale;
    float dz = (lightLevel - fragGroundSmooth);
    float dist = sqrt(distXY * distXY + dz * dz);
    if (dist >= uLightRadii[i])
      continue;

    float visibility =
        pointLightVisibility(vWorldPos, uLightPositions[i], fragGroundSmooth, lightLevel);
    if (visibility <= 0.001)
      continue;

    float reach = clamp(1.0 - dist / uLightRadii[i], 0.0, 1.0);
    float attenuation = reach * reach * reach * (reach * (reach * 6.0 - 15.0) + 10.0);

    vec3 lightVector = vec3(delta.x, delta.y, max(dz, 0.0));
    vec3 pointDir = normalize(lightVector);
    float ndotl = max(dot(normal, pointDir), 0.0);
    float diffuse = mix(0.12, 1.0, pow(ndotl, 0.8));

    float amount = uLightIntensities[i] * attenuation * diffuse * visibility;

    vec3 color = uLightColors[i];
    float maxChannel = max(max(color.r, color.g), color.b);
    if (maxChannel > 0.001)
      color /= maxChannel;
    else
      color = vec3(1.0);

    weightedColor += color * amount;
    totalWeight += amount;
    strongestAmount = max(strongestAmount, amount);
  }

  if (totalWeight <= 0.001)
    return vec3(0.0);

  vec3 blendedColor = weightedColor / totalWeight;
  float cappedAmount = (strongestAmount / (1.0 + strongestAmount)) * 1.65;
  return blendedColor * cappedAmount;
}

// Directional sun occlusion: the same horizon-angle test the point lights use,
// marched along the sun azimuth and compared to the sun's altitude. 1 = lit,
// 0 = shadowed. Analytic against the heightmap, so no bias/acne/peter-panning.
//
// Two user-selectable styles (uShadowSharp):
//  - Smooth: fixed-distance samples read the LINEAR-filtered heightmap, so the
//    occluder horizon ramps between tiles -> soft, rounded shadow edges.
//  - Sharp: a grid DDA visits every tile the ray crosses and reads its centre
//    (the exact discrete elevation) -> blocky, tile-aligned shadow edges.
float sunVisibility(float fragGround)
{
  if (uHeightmapSize.x < 1.0)
    return 1.0;

  vec2 dir = uLightDirection.xy; // horizontal direction toward the sun
  float horiz = length(dir);
  if (horiz < 1.0e-4)
    return 1.0; // sun ~straight up: nothing casts

  dir /= horiz;
  float sunSlope = uLightDirection.z / horiz; // rise (levels) per tile

  float maxAngle = -1.0e9;

  if (uShadowSharp == 1)
  {
    // Grid DDA (Amanatides-Woo) from the fragment toward the sun: visit EVERY
    // tile the ray crosses, in order, reading each tile's centre. Visiting every
    // crossed tile (vs fixed-distance samples) means no occluder is skipped, so a
    // block casts one coherent silhouette instead of scattered bits.
    ivec2 tile = ivec2(floor(vWorldPos));
    ivec2 stepDir = ivec2(sign(dir.x), sign(dir.y));
    vec2 invDir = 1.0 / max(abs(dir), vec2(1.0e-6));
    vec2 tMax;
    tMax.x = (dir.x >= 0.0 ? float(tile.x) + 1.0 - vWorldPos.x
                           : vWorldPos.x - float(tile.x)) *
             invDir.x;
    tMax.y = (dir.y >= 0.0 ? float(tile.y) + 1.0 - vWorldPos.y
                           : vWorldPos.y - float(tile.y)) *
             invDir.y;

    const int MAX_STEPS = 28;
    const float kSunShadowMaxDist = 18.0; // tiles
    for (int i = 0; i < MAX_STEPS; i++)
    {
      // Step to the next tile boundary (the fragment's own tile is passed first,
      // so it never self-occludes).
      if (tMax.x < tMax.y)
      {
        tile.x += stepDir.x;
        tMax.x += invDir.x;
      }
      else
      {
        tile.y += stepDir.y;
        tMax.y += invDir.y;
      }

      vec2 center = vec2(tile) + 0.5;
      float dist = length(center - vWorldPos);
      if (dist > kSunShadowMaxDist)
        break;

      float th = terrainLevelAt(center);
      if (th < -1.0e8)
        continue;

      maxAngle = max(maxAngle, (th - fragGround) / dist);
    }

    // Tight penumbra keeps the blocky silhouette crisp.
    return 1.0 - smoothstep(0.0, 0.06, maxAngle - sunSlope);
  }

  // Smooth: even point samples against the linearly-filtered heightmap.
  const int STEPS = 24;
  const float kSunShadowMaxDist = 16.0; // tiles
  for (int s = 0; s < STEPS; s++)
  {
    float d = kSunShadowMaxDist * (float(s) + 1.0) / float(STEPS);
    if (d < 0.5)
      continue;
    float th = terrainLevelAt(vWorldPos + dir * d);
    if (th < -1.0e8)
      continue;
    maxAngle = max(maxAngle, (th - fragGround) / d);
  }

  // Wider penumbra for soft, rounded edges.
  return 1.0 - smoothstep(0.0, 0.30, maxAngle - sunSlope);
}

void main()
{
  // Cutaway: hide the roof above the player, and (when localized) everything
  // outside a radius of them, so only the cave around the player shows.
  if (vGround > uClipElevation)
    discard;
  if (uClipRadius > 0.0 && distance(vWorldPos, uClipCenter) > uClipRadius)
    discard;

  vec3 normal = normalize(vNormal);

  vec4 albedo = texture(uTexture, vUv);

  // Surface shimmer applies to top faces (real up-facing).
  if (normal.z > 0.5)
  {
    if (uSurfaceEffect == 3)
      albedo.rgb = applyGrassEffect(albedo.rgb, vUv, vWorldPos);
    else if (uSurfaceEffect == 4)
      albedo.rgb = applySandEffect(albedo.rgb, vUv, vWorldPos);
  }

  if (albedo.a <= 0.0)
    discard;

  vec3 lightDir = normalize(uLightDirection);
  float sunDiffuse = max(dot(normal, lightDir), 0.0);

  // Real geometry: "up-ness" is the world Z of the normal (top faces ~1, sides ~0).
  float upness = smoothstep(0.0, 0.5, normal.z);
  float sunFacing = smoothstep(0.0, 0.6, sunDiffuse);
  float shade = (1.0 - upness) * (1.0 - sunFacing);

  float sunHeight = clamp(lightDir.z, 0.0, 1.0);
  float ambientLevel = uAmbient * mix(mix(0.8, 1.0, sunHeight), 1.0, 1.0 - upness);

  float litLevel = ambientLevel + sunDiffuse * uDiffuseStrength;
  float shadedLevel = min(1.0 - uDiffuseStrength, uAmbient);
  float lit = mix(litLevel, shadedLevel, shade);

  // Cast sun shadows pull the fragment toward its shaded (ambient-only) level,
  // so a shadowed surface keeps ambient but loses the directional sun. Disabled
  // when the projected shadow technique is selected.
  if (uSunShadowEnabled == 1)
    lit = mix(shadedLevel, lit, sunVisibility(vGround));

  vec3 sunlight = vec3(lit) * uLightColor;
  sunlight = max(sunlight, vec3(0.03));

  float daylight = smoothstep(0.20, 0.75, uAmbient);
  float pointVisibility = 1.0 - daylight;
  pointVisibility *= pointVisibility;

  vec3 pointLight = vec3(0.0);
  if (pointVisibility > 0.001)
    pointLight = calculatePointLighting(normal) * (pointVisibility * 2.0);

  float sunlightAmount = max(max(sunlight.r, sunlight.g), sunlight.b);
  float shadowRoom =
      mix(1.0, 1.0 - smoothstep(0.65, 1.0, sunlightAmount), daylight);

  vec3 totalLight = clamp(sunlight + pointLight * shadowRoom, 0.0, 1.0);

  FragColor = vec4(albedo.rgb * totalLight, albedo.a);
}
