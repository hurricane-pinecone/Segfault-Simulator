#define MAX_LIGHTS 128

in vec2 vUv;
in vec4 vColor;
in vec2 vWorldPos;
in float vGround;

out vec4 FragColor;

uniform sampler2D uTexture;

uniform float uAmbient;
uniform vec3 uAmbientColor;
uniform float uHeightScale;

// Sun shadow: a stain darkens in the same cast shadows as the surface under it,
// using the heightmap horizon-march (matching geometry.frag / the lit shader).
uniform vec3 uLightDirection;
uniform float uDiffuseStrength;
uniform int uSunShadowEnabled;
uniform int uShadowSharp;

uniform sampler2D uHeightmap;
uniform vec2 uHeightmapOrigin;
uniform vec2 uHeightmapSize;
uniform float uHeightmapTexSize;

uniform int uLightCount;
uniform vec2 uLightPositions[MAX_LIGHTS];
uniform vec3 uLightColors[MAX_LIGHTS];
uniform float uLightIntensities[MAX_LIGHTS];
uniform float uLightRadii[MAX_LIGHTS];
uniform float uLightHeights[MAX_LIGHTS];
uniform float uLightGroundLevels[MAX_LIGHTS];

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

// Directional sun occlusion, identical to geometry.frag's march so a stain's
// shadow lines up exactly with the terrain's. 1 = lit, 0 = shadowed.
float sunVisibility(float fragGround)
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

  if (uShadowSharp == 1)
  {
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
    const float kSunShadowMaxDist = 18.0;
    for (int i = 0; i < MAX_STEPS; i++)
    {
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

    return 1.0 - smoothstep(0.0, 0.06, maxAngle - sunSlope);
  }

  const int STEPS = 24;
  const float kSunShadowMaxDist = 16.0;
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

  return 1.0 - smoothstep(0.0, 0.30, maxAngle - sunSlope);
}

// Point-light contribution, matching the lit shader's model (3D distance with
// the elevation gap, smootherstep falloff, colour-normalised blend) but without
// the terrain horizon occlusion -- a decal lies on a surface, and skipping the
// per-pixel march keeps it cheap. Normal is treated as up (ground-facing).
vec3 decalPointLight()
{
  vec3 weightedColor = vec3(0.0);
  float totalWeight = 0.0;
  float strongest = 0.0;

  for (int i = 0; i < MAX_LIGHTS; i++)
  {
    if (i >= uLightCount)
      break;

    vec2 delta = uLightPositions[i] - vWorldPos;
    float distXY = length(delta);
    if (distXY >= uLightRadii[i])
      continue;

    float lightLevel = uLightGroundLevels[i] + uLightHeights[i] * uHeightScale;
    float dz = lightLevel - vGround;
    float dist = sqrt(distXY * distXY + dz * dz);
    if (dist >= uLightRadii[i])
      continue;

    float reach = clamp(1.0 - dist / uLightRadii[i], 0.0, 1.0);
    float attenuation =
        reach * reach * reach * (reach * (reach * 6.0 - 15.0) + 10.0);

    vec3 pointDir = normalize(vec3(delta.x, delta.y, max(dz, 0.0)));
    float ndotl = max(pointDir.z, 0.0); // surface normal up
    float diffuse = mix(0.12, 1.0, pow(ndotl, 0.8));

    float amount = uLightIntensities[i] * attenuation * diffuse;

    vec3 color = uLightColors[i];
    float mc = max(max(color.r, color.g), color.b);
    color = mc > 0.001 ? color / mc : vec3(1.0);

    weightedColor += color * amount;
    totalWeight += amount;
    strongest = max(strongest, amount);
  }

  if (totalWeight <= 0.001)
    return vec3(0.0);

  vec3 blended = weightedColor / totalWeight;
  float capped = (strongest / (1.0 + strongest)) * 1.65;
  return blended * capped;
}

void main()
{
  vec4 tex = texture(uTexture, vUv) * vColor;
  if (tex.a <= 0.0)
    discard;

  // Point lights only assert themselves as ambient drops (same gating as the lit
  // shader), so blood near a lamp lights up at night but day ambient dominates.
  float daylight = smoothstep(0.20, 0.75, uAmbient);
  float pointVisibility = 1.0 - daylight;
  pointVisibility *= pointVisibility;

  vec3 pointLight = vec3(0.0);
  if (pointVisibility > 0.001)
    pointLight = decalPointLight() * (pointVisibility * 2.0);

  // A cast shadow pulls the stain down to its shaded (ambient-only) level, the
  // same drop the surface beneath it takes, so blood reads as painted on.
  float ambient = uAmbient;
  if (uSunShadowEnabled == 1)
  {
    float shadedLevel = min(1.0 - uDiffuseStrength, uAmbient);
    ambient = mix(shadedLevel, uAmbient, sunVisibility(vGround));
  }

  vec3 lighting = uAmbientColor * ambient + pointLight;

  FragColor = vec4(tex.rgb * lighting, tex.a);
}
