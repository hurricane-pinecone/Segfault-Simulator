// Isometric implementations of quad.frag's variant hooks: heightfield terrain
// sun shadows, point-light terrain occlusion, and the iso surface effects
// (grass/sand). Appended after quad.frag for the isometric renderer. Relies on
// the common uniforms + MAX_LIGHTS declared in quad.frag.

uniform int uSunShadowEnabled; // 1 = cast terrain shadows via the heightmap march

uniform float uLightHeights[MAX_LIGHTS];
uniform float uLightGroundLevels[MAX_LIGHTS]; // terrain level under each light

uniform int uSurfaceEffect;

// Terrain heightmap: one texel per tile holding the tile elevation in levels.
uniform sampler2D uHeightmap;
uniform vec2 uHeightmapOrigin;    // min tile, world space
uniform vec2 uHeightmapSize;      // valid grid dimensions in tiles (0 = disabled)
uniform float uHeightmapTexSize;  // allocated texture dimension (>= grid)
uniform float uHeightScale;       // light emitter height -> elevation levels

// Terrain elevation (levels) at a world-space position, or a very low value
// outside the grid so it never occludes. The grid lives in the corner of a
// larger fixed texture, so normalize tile coords by the texture size.
float terrainLevelAt(vec2 world)
{
  if (uHeightmapSize.x < 1.0 || uHeightmapSize.y < 1.0)
    return -1e9;

  vec2 texel = world - uHeightmapOrigin; // grid tile coordinates

  if (texel.x < 0.0 || texel.x >= uHeightmapSize.x || texel.y < 0.0 ||
      texel.y >= uHeightmapSize.y)
    return -1e9;

  return texture(uHeightmap, texel / uHeightmapTexSize).r;
}

// How much of the light reaches the fragment, 0 (fully blocked) .. 1 (clear).
// Terrain taller than the lamp strictly between the two blocks it, i.e. a
// mountain taller than the lamp casts a shadow.
//
// Horizon-angle test: terrain blocks the light where its elevation ANGLE seen
// from the fragment's ground (height gained per tile of distance) rises above the
// angle to the light. Anchoring every occluder to the fragment this way means a
// flat ridge (angle 0) still beats a light sitting below the fragment (negative
// angle), so a fragment high on a plateau is correctly shadowed and the far side
// stays dark.
float pointLightVisibility(vec2 fragXY, vec2 lightXY, float fragGround,
                           float lightLevel)
{
  if (uHeightmapSize.x < 1.0)
    return 1.0;

  // A fragment outside the grid has no valid ground; fall back to a flat plane at
  // the light's height there.
  float anchor = fragGround < -1.0e8 ? lightLevel : fragGround;

  vec2 toLight = lightXY - fragXY;
  float distTotal = length(toLight);

  if (distTotal < 1.0e-4)
    return 1.0;

  // Angle (levels per tile of horizontal distance) from the fragment's ground up
  // to the light. Negative when the light sits below the fragment.
  float lightAngle = (lightLevel - anchor) / distTotal;

  // Dense, evenly-spaced samples (no per-fragment jitter). Jitter dithers away
  // streaks for a static light, but for a moving light it turns the penumbra
  // into noise that shimmers along the shadow edge as the light moves. A finer
  // march removes the streaks without that flicker.
  const int STEPS = 48;

  float maxAngle = -1.0e9;

  for (int s = 0; s < STEPS; s++)
  {
    float t = (float(s) + 0.5) / float(STEPS);
    float d = distTotal * t;

    // Ignore terrain within ~one tile of the fragment so it can't self-occlude.
    // A smooth distance cutoff (rather than a per-tile test) keeps the shadow a
    // continuous function of position, with no seam as the fragment crosses a tile.
    if (d < 0.85)
      continue;

    vec2 samplePos = mix(fragXY, lightXY, t);
    float terrainAngle = (terrainLevelAt(samplePos) - anchor) / d;
    maxAngle = max(maxAngle, terrainAngle);
  }

  // How far (in angle) the highest occluder rises above the line to the light.
  // Feathering in ANGLE space keeps the penumbra a consistent softness at any
  // light distance (the level-equivalent width would shrink with range and read
  // as a hard edge far from the light). The wide band also smears the per-tile
  // anchor steps into a gradient, so the lit->dark transition is gradual.
  float over = maxAngle - lightAngle;
  return 1.0 - smoothstep(0.0, 0.30, over);
}

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

float isoTopMask(vec2 uv)
{
  vec2 p = uv - vec2(0.5, 0.25);

  float diamond =
      abs(p.x) / 0.5 +
      abs(p.y) / 0.25;

  return 1.0 - smoothstep(0.92, 1.02, diamond);
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

  float blade =
      smoothstep(0.72, 0.92, valueNoise(vec2(p.x * 0.7, p.y * 3.5)));

  color *= mix(vec3(1.0), vec3(0.75, 1.12, 0.65), blade * 0.35);

  return color;
}

vec3 applySandEffect(vec3 color, vec2 uv, vec2 worldPos)
{
  vec2 p =
      worldPos * 18.0 +
      uv * 72.0;

  // Rotate-ish coordinate basis for wind-shaped diagonal dunes.
  float along =
      p.x * 0.85 + p.y * 0.32;

  float across =
      p.x * -0.28 + p.y * 0.96;

  float warp =
      valueNoise(vec2(along * 0.08, across * 0.18)) * 2.0 - 1.0;

  float dune =
      sin(along * 0.42 + warp * 2.8) * 0.5 + 0.5;

  dune =
      smoothstep(0.38, 0.72, dune);

  float fineDune =
      sin(along * 1.4 + warp * 3.5) * 0.5 + 0.5;

  fineDune =
      smoothstep(0.48, 0.82, fineDune);

  float grain =
      valueNoise(p * 3.7);

  float specks =
      smoothstep(0.90, 0.985, valueNoise(p * 10.0));

  vec3 sand = color;

  // broad dune bands
  sand *= mix(0.90, 1.10, dune * 0.25);

  // smaller wind ripples
  sand *= mix(0.96, 1.06, fineDune * 0.15);

  // fine grain
  sand *= mix(0.97, 1.03, grain * 0.5);

  // sparse darker flecks
  sand -= specks * 0.025;

  return sand;
}

// Directional sun occlusion against the terrain heightmap: 1 = lit, 0 = in a
// cast terrain shadow. Marches from the fragment along the sun azimuth and
// compares the terrain's max horizon angle to the sun's altitude -- the same
// horizon test the point lights use, fixed to the sun.
float sunVisibility()
{
  if (uHeightmapSize.x < 1.0)
    return 1.0;

  vec2 dir = uLightDirection.xy;
  float horiz = length(dir);
  if (horiz < 1.0e-4)
    return 1.0;

  dir /= horiz;
  float sunSlope = uLightDirection.z / horiz; // rise (levels) per tile

  float fragGround = terrainLevelAt(vWorldPosition);
  if (fragGround < -1.0e8)
    return 1.0; // outside the heightmap window

  const int STEPS = 24;
  const float kSunShadowMaxDist = 16.0; // tiles
  float maxAngle = -1.0e9;
  for (int s = 0; s < STEPS; s++)
  {
    float d = kSunShadowMaxDist * (float(s) + 1.0) / float(STEPS);
    if (d < 0.5)
      continue;
    float th = terrainLevelAt(vWorldPosition + dir * d);
    if (th < -1.0e8)
      continue;
    maxAngle = max(maxAngle, (th - fragGround) / d);
  }

  return 1.0 - smoothstep(0.0, 0.30, maxAngle - sunSlope);
}

// --- quad.frag hooks ---------------------------------------------------------

vec3 surfaceEffect(vec3 albedo, vec2 uv, vec2 worldPos)
{
  float top = isoTopMask(uv);

  if (uSurfaceEffect == 3) // Grass
    albedo = mix(albedo, applyGrassEffect(albedo, uv, worldPos), top);
  else if (uSurfaceEffect == 4) // Sand
    albedo = mix(albedo, applySandEffect(albedo, uv, worldPos), top);

  return albedo;
}

// Cast terrain shadows (heightmap horizon-march) pull the fragment toward its
// shaded level, so a tile or sprite in another block's shadow loses direct sun
// but keeps ambient. Disabled when the projected shadow technique is selected.
float sunShadow(float lit, float shadedLevel)
{
  if (uSunShadowEnabled == 1)
    return mix(shadedLevel, lit, sunVisibility());

  return lit;
}

vec3 pointLighting(vec3 normal)
{
  vec3 weightedColor = vec3(0.0);
  float totalWeight = 0.0;
  float strongestAmount = 0.0;

  // The fragment's ground feeds both the occlusion ray and the distance
  // attenuation. Sample it BILINEARLY so every term is a continuous function of
  // position -- the lighting then flows smoothly across the terrain instead of
  // snapping to the tile grid (a per-tile sample makes each elevation step a hard
  // brightness seam). Same for every light, so sample once.
  float fragGroundSmooth = terrainLevelAt(vWorldPosition);

  for (int i = 0; i < MAX_LIGHTS; i++)
  {
    if (i >= uLightCount)
      break;

    vec2 delta = uLightPositions[i] - vWorldPosition;
    float distXY = length(delta);

    // Cheap horizontal reject (the full 3D distance only grows from here).
    if (distXY >= uLightRadii[i])
      continue;

    // Terrain between the fragment and the light blocks it, the same way a
    // mountain blocks the sun. The light sits at its emitter height (in levels)
    // above its ground. The ground level is supplied per light by the CPU (it
    // eases between tile elevations for a moving emitter), not sampled from the
    // heightmap here -- a heightmap sample would snap a whole level as the light
    // crossed a tile border and make the lit area pop.
    float lightLevel =
        uLightGroundLevels[i] + uLightHeights[i] * uHeightScale;

    // A point light is 3D: fold the elevation gap between the light and the
    // fragment's ground into the distance. Without this the light is a flat disc
    // that lights a whole mountain face at full strength just because its XY
    // distance is small -- it "climbs" and bleeds over tall terrain regardless of
    // height. LEVEL_TO_TILE is how many tiles of reach one elevation level costs;
    // higher = the light hugs the ground more and climbs tall terrain less.
    const float LEVEL_TO_TILE = 1.0;
    float fragGroundSafe =
        fragGroundSmooth < -1.0e8 ? lightLevel : fragGroundSmooth;
    float dz = (lightLevel - fragGroundSafe) * LEVEL_TO_TILE;
    float dist = sqrt(distXY * distXY + dz * dz);

    if (dist >= uLightRadii[i])
      continue;

    // The occlusion anchor is the bilinear ground, so the shadow is a continuous
    // function of position with no per-tile seam. The bilinear lift at a cliff
    // foot is harmless: the horizon-angle test still sees the adjacent cliff at a
    // steep angle and shadows it, and the wide penumbra hides the small residual.
    float visibility = pointLightVisibility(
        vWorldPosition, uLightPositions[i], fragGroundSmooth, lightLevel);

    if (visibility <= 0.001)
      continue;

    // Smootherstep falloff (3rd-order ease at both ends) so the pool fades into
    // darkness with no hard ring at the radius and no abrupt onset.
    float reach = clamp(1.0 - dist / uLightRadii[i], 0.0, 1.0);
    float attenuation = reach * reach * reach * (reach * (reach * 6.0 - 15.0) + 10.0);

    // The light vector's vertical leg is the elevation gap dz (in tiles), shared
    // with the 3D distance above, so the diffuse direction stays in world-tile
    // units and a taller emitter tilts the light more overhead.
    vec3 lightVector = vec3(delta.x, delta.y, max(dz, 0.0));
    vec3 pointDir = normalize(lightVector);

    float ndotl = max(dot(normal, pointDir), 0.0);
    float diffuse = mix(0.12, 1.0, pow(ndotl, 0.8));

    float amount =
        uLightIntensities[i] *
        attenuation *
        diffuse *
        visibility;

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
