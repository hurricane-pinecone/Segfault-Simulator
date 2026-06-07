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

uniform int uLightCount;
uniform vec2 uLightPositions[MAX_LIGHTS];
uniform vec3 uLightColors[MAX_LIGHTS];
uniform float uLightIntensities[MAX_LIGHTS];
uniform float uLightRadii[MAX_LIGHTS];
uniform float uLightHeights[MAX_LIGHTS];
uniform float uLightGroundLevels[MAX_LIGHTS];

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

  vec3 lighting = uAmbientColor * uAmbient + pointLight;

  FragColor = vec4(tex.rgb * lighting, tex.a);
}
