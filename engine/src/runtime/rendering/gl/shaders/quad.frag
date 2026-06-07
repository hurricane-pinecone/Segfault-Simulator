// Shared lit textured-quad fragment shader: the common 2D lighting and main().
// Three hooks (surfaceEffect / sunShadow / pointLighting) are implemented by a
// variant appended after this file: quadBase.frag for the flat-2D renderer, or
// isometric/quadIso.frag for the heightfield terrain renderer. The renderer
// concatenates `glslVersion + quad.frag + <variant>` at shader build time.

in vec2 vUv;
in vec2 vWorldPosition;
in vec4 vTint;

out vec4 FragColor;

#define MAX_LIGHTS 128

uniform sampler2D uTexture;
uniform sampler2D uNormalTexture;

uniform vec4 uColor;

uniform int uUseLighting;
uniform int uHasNormalMap;

uniform vec3 uLightDirection;
uniform vec3 uLightColor; // sun/ambient scene tint
uniform float uAmbient;
uniform float uDiffuseStrength;

uniform int uLightCount;
uniform vec2 uLightPositions[MAX_LIGHTS];
uniform vec3 uLightColors[MAX_LIGHTS];
uniform float uLightIntensities[MAX_LIGHTS];
uniform float uLightRadii[MAX_LIGHTS];

// Variant hooks, defined by the appended base/iso file:
//   surfaceEffect - tint the albedo before lighting (iso grass/sand; 2D no-op).
//   sunShadow     - fold a cast terrain shadow into the lit level (iso; 2D no-op).
//   pointLighting - accumulate the scene's point lights for this fragment.
vec3 surfaceEffect(vec3 albedo, vec2 uv, vec2 worldPos);
float sunShadow(float lit, float shadedLevel);
vec3 pointLighting(vec3 normal);

void main()
{
  vec4 albedo = texture(uTexture, vUv);
  albedo *= vTint; // per-quad tint (white by default -> no change)

  albedo.rgb = surfaceEffect(albedo.rgb, vUv, vWorldPosition);

  if (albedo.a <= 0.0)
    discard;

  if (uUseLighting == 0)
  {
    FragColor = albedo * uColor;
    return;
  }

  vec3 normal = vec3(0.0, 0.0, 1.0);
  float emissiveMask = 0.0;

  if (uHasNormalMap != 0)
  {
    vec3 normalSample = texture(uNormalTexture, vUv).rgb;

    emissiveMask = smoothstep(
        0.95,
        1.0,
        min(min(normalSample.r, normalSample.g), normalSample.b));

    normal = normalSample * 2.0 - 1.0;

    if (length(normal) < 0.001)
      normal = vec3(0.0, 0.0, 1.0);
    else
      normal = normalize(normal);
  }

  vec3 lightDir = normalize(uLightDirection);
  float sunDiffuse = max(dot(normal, lightDir), 0.0);

  // These normal maps are screen-space: every face shares roughly the same z, so
  // tops and sides are told apart by the green channel (normal.y = "up on
  // screen") — tops point up (~+0.8), side faces point down (~-0.4). Only a side
  // face that is ALSO turned away from the sun is shaded, so terrain tops stay
  // bright whatever the sun angle.
  float upness = smoothstep(0.0, 0.5, normal.y);
  float sunFacing = smoothstep(0.0, 0.6, sunDiffuse);
  float shade = (1.0 - upness) * (1.0 - sunFacing);

  // Top faces dim slightly as the sun drops toward the horizon: a higher sun
  // (lightDir.z) lets an up-facing surface catch more sky light. The top normal
  // barely faces the sun horizontally, so tie this to the sun's elevation rather
  // than the dot product. Kept subtle - noon tops read full, low-sun tops only
  // soften - and applied only to up-facing surfaces so side faces are untouched.
  float sunHeight = clamp(lightDir.z, 0.0, 1.0);
  float ambientLevel = uAmbient * mix(mix(0.8, 1.0, sunHeight), 1.0, 1.0 - upness);

  // Lit faces get ambient + direct sun. A fully shaded side drops to the same
  // brightness a cast terrain shadow leaves it at (direct sun removed), so a
  // sprite's dark side reads as dark as the shadows on the ground. 1.0 -
  // uDiffuseStrength mirrors the terrain shadow overlay (terrainShadowAlpha
  // defaults to 1); never brighter than the ambient sky term.
  float litLevel = ambientLevel + sunDiffuse * uDiffuseStrength;
  float shadedLevel = min(1.0 - uDiffuseStrength, uAmbient);

  float lit = mix(litLevel, shadedLevel, shade);

  lit = sunShadow(lit, shadedLevel);

  vec3 sunlight = vec3(lit) * uLightColor;

  sunlight = max(sunlight, vec3(0.03));

  // How much point lights assert themselves, driven purely by the ambient sky
  // level (uAmbient) -- NOT a day/night clock. Bright ambient drowns them out;
  // any time ambient drops they fade back in, so a storm darkening the sky at
  // midday lets point lights and their occlusion work normally.
  float daylight =
      smoothstep(0.20, 0.75, uAmbient);

  float pointVisibility =
      1.0 - daylight;

  pointVisibility =
      pointVisibility * pointVisibility;

  // Skip the point-light pass entirely when its contribution would round to
  // nothing -- it can be expensive (the iso variant runs a per-light occlusion
  // march). The test is on uAmbient (uniform), so the branch is coherent.
  vec3 pointLight = vec3(0.0);

  if (pointVisibility > 0.001)
    pointLight = pointLighting(normal) * (pointVisibility * 2.0);

  float sunlightAmount =
      max(max(sunlight.r, sunlight.g), sunlight.b);

  // At night: point lights have full authority.
  // During day: point lights only affect shadowed areas.
  float shadowRoom =
      mix(
          1.0,
          1.0 - smoothstep(0.65, 1.0, sunlightAmount),
          daylight);

  vec3 totalLight =
      sunlight + pointLight * shadowRoom;

  totalLight =
      clamp(totalLight, 0.0, 1.0);

  vec3 litColor =
      albedo.rgb * totalLight;

  vec3 emissiveColor =
      albedo.rgb * 2.5;

  vec3 finalRgb =
      mix(litColor, emissiveColor, emissiveMask);

  FragColor =
      vec4(finalRgb, albedo.a) * uColor;
}
