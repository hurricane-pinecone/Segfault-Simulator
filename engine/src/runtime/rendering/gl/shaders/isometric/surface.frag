#define MAX_LIGHTS 128

in vec2 vWorldPosition;
in vec4 vColor;
in vec2 vUv;
in vec4 vParams;

out vec4 FragColor;

uniform float uTime;
uniform float uRippleStrength;
uniform float uAmbient;

uniform int uLightCount;
uniform vec2 uLightPositions[MAX_LIGHTS];
uniform vec3 uLightColors[MAX_LIGHTS];
uniform float uLightIntensities[MAX_LIGHTS];
uniform float uLightRadii[MAX_LIGHTS];

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

void main()
{
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

  vec3 pointLight = vec3(0.0);

  for (int i = 0; i < MAX_LIGHTS; i++)
  {
    if (i >= uLightCount)
      break;

    float dist = length(uLightPositions[i] - vWorldPosition);

    if (dist >= uLightRadii[i])
      continue;

    float attenuation = 1.0 - dist / uLightRadii[i];
    attenuation = clamp(attenuation, 0.0, 1.0);
    attenuation = pow(attenuation, 1.35);

    pointLight +=
        uLightColors[i] *
        uLightIntensities[i] *
        attenuation *
        0.45;
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

  float pointLightAmount =
      max(max(pointLight.r, pointLight.g), pointLight.b);

  float sunCausticVisibility =
      smoothstep(0.18, 0.75, uAmbient);

  float pointCausticVisibility =
      smoothstep(0.02, 0.35, pointLightAmount);

  float causticVisibility =
      max(sunCausticVisibility, pointCausticVisibility);

  float causticStrength =
      0.25 * shallowFactor;

  causticStrength *=
      1.0 + clamp(pointLightAmount, 0.0, 1.0) * 2.5;

  vec3 causticColor = vec3(0.85, 0.97, 1.0);
  float ambientVisibility = mix(0.25, 1.0, clamp(uAmbient, 0.0, 1.0));

  float lightFloor = mix(0.06, 0.82, clamp(uAmbient, 0.0, 1.0));
  color *= max(uAmbient, lightFloor);

  // Keep transparent water readable at night.
  vec3 nightWaterFloor = vColor.rgb * 0.22;
  color = max(color, nightWaterFloor);

  color +=
      causticColor *
      caustic *
      causticStrength *
      causticVisibility;

  color += pointLight;
  color = clamp(color, 0.0, 1.0);

  FragColor = vec4(color, vColor.a);
}
