// Flat-lit coloured cubes (the Teardown look): a single directional term over an
// ambient floor, shaded by the flat face normal so each face reads as a facet.
// #version (+ ES precision) is prepended at runtime.
#define MAX_LIGHTS 16

in vec3 vWorldPos;
in vec3 vNormal;
in vec4 vColor;

out vec4 FragColor;

uniform vec3 uLightDir;    // direction TOWARD the sun, world space
uniform vec3 uSunColor;    // sun/sky tint (day white, dusk orange, night blue)
uniform float uAmbient;    // ambient floor (high day, low night)
uniform float uSunDiffuse; // directional strength (~0 at night)
uniform float uEmissive;   // > 0.5: skip lighting (flames glow at their colour)

uniform int uLightCount;
uniform vec3 uLightPos[MAX_LIGHTS];
uniform vec3 uLightColor[MAX_LIGHTS];
uniform float uLightRadius[MAX_LIGHTS];
uniform float uLightIntensity[MAX_LIGHTS];

void main()
{
  if (uEmissive > 0.5)
  {
    FragColor = vColor; // unlit, full-bright -- additive blend makes it glow
    return;
  }

  vec3 n = normalize(vNormal);

  // Sky/sun light: ambient floor + directional, tinted by the time-of-day sun
  // colour. At night uAmbient + uSunDiffuse fall, so point lights take over.
  float sun = max(dot(n, normalize(uLightDir)), 0.0);
  vec3 lit = vColor.rgb * uSunColor * (uAmbient + uSunDiffuse * sun);

  // Point lights: 3D distance attenuation + N.L, added on top.
  for (int i = 0; i < uLightCount; i++)
  {
    vec3 toLight = uLightPos[i] - vWorldPos;
    float dist = length(toLight);
    if (dist >= uLightRadius[i])
      continue;
    float atten = 1.0 - dist / uLightRadius[i];
    atten *= atten; // soft falloff
    float ndl = max(dot(n, toLight / max(dist, 0.0001)), 0.0);
    lit += vColor.rgb * uLightColor[i] * uLightIntensity[i] * atten * ndl;
  }

  FragColor = vec4(lit, vColor.a);
}
