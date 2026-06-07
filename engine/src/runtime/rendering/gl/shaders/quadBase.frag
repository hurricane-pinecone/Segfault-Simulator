// Pure-2D implementations of quad.frag's variant hooks: no terrain, no
// heightfield. Appended after quad.frag for the flat (non-isometric) renderer.

// No terrain surface effects in 2D.
vec3 surfaceEffect(vec3 albedo, vec2 uv, vec2 worldPos)
{
  return albedo;
}

// No cast terrain shadows in 2D.
float sunShadow(float lit, float shadedLevel)
{
  return lit;
}

// 2D point lights: lights and fragments share one plane, so distance is planar
// and there is no terrain to occlude them.
vec3 pointLighting(vec3 normal)
{
  vec3 weightedColor = vec3(0.0);
  float totalWeight = 0.0;
  float strongestAmount = 0.0;

  for (int i = 0; i < MAX_LIGHTS; i++)
  {
    if (i >= uLightCount)
      break;

    vec2 delta = uLightPositions[i] - vWorldPosition;
    float dist = length(delta);

    if (dist >= uLightRadii[i])
      continue;

    // Smootherstep falloff (3rd-order ease at both ends) so the pool fades into
    // darkness with no hard ring at the radius and no abrupt onset.
    float reach = clamp(1.0 - dist / uLightRadii[i], 0.0, 1.0);
    float attenuation = reach * reach * reach * (reach * (reach * 6.0 - 15.0) + 10.0);

    vec3 pointDir = normalize(vec3(delta.x, delta.y, 0.0));
    float ndotl = max(dot(normal, pointDir), 0.0);
    float diffuse = mix(0.12, 1.0, pow(ndotl, 0.8));

    float amount = uLightIntensities[i] * attenuation * diffuse;

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

  float cappedAmount =
      strongestAmount / (1.0 + strongestAmount);

  cappedAmount *= 1.65;

  return blendedColor * cappedAmount;
}
