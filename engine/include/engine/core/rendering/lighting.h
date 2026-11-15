#pragma once

#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"

#include <cstdint>

namespace sfs
{

/**
 * The directional sun.
 *
 * @param direction       vector pointing toward the sun (need not be unit
 *                        length); Z is up, so Z <= 0 means the sun has set
 * @param color           linear RGB tint of sunlight
 * @param diffuseStrength how much a surface facing the sun brightens over
 *                        ambient (0..1)
 */
struct SunLight
{
  glm::vec3 direction{-0.45f, -0.35f, 0.82f};
  glm::vec3 color{1.0f, 0.96f, 0.88f};
  float diffuseStrength = 0.85f;
};

/**
 * The moon. Once the sun has set it takes over as the light that casts
 * shadows, as bright as moonlight() says; a physical sky draws its disk and
 * glows brighter under it.
 *
 * @param direction       vector pointing toward the moon (need not be unit
 *                        length); Z <= 0: below the horizon (the default: no
 *                        moon)
 * @param phase           how much of its face is lit: 0 new, 0.5 a half
 *                        moon, 1 full
 * @param color           linear RGB tint of its light, under a flat sky
 * @param diffuseStrength how much a surface facing a full moon high in a
 *                        clear sky brightens over ambient, under a flat sky
 *                        (a physical sky works its light out itself)
 */
struct MoonLight
{
  glm::vec3 direction{0.0f, 0.0f, -1.0f};
  float phase = 1.0f;
  glm::vec3 color{0.62f, 0.72f, 1.0f};
  float diffuseStrength = 0.12f;
};

/**
 * Scene-wide lighting: the sun and moon, plus the ambient level and sky
 * colour.
 *
 * @param ambient    base brightness everything receives (0 = black night,
 *                   1 = fully lit); also gates point lights, which fade in as
 *                   ambient drops
 * @param skyColor   linear RGB background clear colour
 * @param cloudCover how overcast it is (0 clear .. 1 overcast): dims the sun's
 *                   and the moon's light and hides the stars and the moon's
 *                   disk (the clouds themselves aren't drawn yet)
 */
struct SceneLighting
{
  SunLight sun;
  MoonLight moon;
  float ambient = 0.55f;
  glm::vec3 skyColor{0.55f, 0.72f, 0.90f};
  float cloudCover = 0.0f;
};

/** How much of the sun's or moon's light gets through `cloudCover` (0..1). */
float clearSky(float cloudCover);

/**
 * How strongly the moon lights the scene, 0..1 (1: a full moon high in a
 * clear sky, the sun well set): brighter the fuller it is, fading as it
 * nears the horizon, and in as the sun sinks through twilight (from its
 * setting to about 6 degrees below), behind clouds.
 */
float moonlight(const SceneLighting& lighting);

/**
 * One point light, resolved to world space for a frame.
 *
 * @param position  world position
 * @param color     linear RGB tint
 * @param intensity brightness multiplier (0 = off)
 * @param radius    reach in world units; contributes nothing beyond it
 * @param castsShadow whether things block it (if it's among the frame's
 *                  shadowed lights; see ShadowSettings::pointLights)
 * @param shadowNear what's within this distance of it (along any axis: a box
 *                  around it) doesn't block it; for a glowing object's light,
 *                  the object's own glowing surface
 * @param sourceRadius how big what gives off the light is (0: a point): shiny
 *                  surfaces reflect a highlight that big, as a sphere light's
 * @param spotDirection a spot light's axis, unit length, the way it shines
 *                  (zero: not a spot, it shines all round)
 * @param spotCosInner the cosine of a spot's full-strength core's angle from
 *                  its axis
 * @param spotCosOuter the cosine of the angle from its axis where a spot's
 *                  light has faded out (at most spotCosInner)
 */
struct PointLight
{
  glm::vec3 position{0.0f};
  glm::vec3 color{1.0f, 0.85f, 0.6f};
  float intensity = 1.0f;
  float radius = 8.0f;
  bool castsShadow = false;
  float shadowNear = 0.05f;
  float sourceRadius = 0.0f;
  glm::vec3 spotDirection{0.0f};
  float spotCosInner = -1.0f;
  float spotCosOuter = -1.0f;

  bool spot() const { return spotDirection != glm::vec3(0.0f); }
};

/**
 * Make `light` a spot along `direction` (any length; zero: a point light
 * again), full strength within `innerDegrees` of it, faded out by
 * `outerDegrees` (both from the axis, clamped to 0..180; outer at least
 * inner).
 */
void setSpot(PointLight& light,
             glm::vec3 direction,
             float innerDegrees,
             float outerDegrees);

/**
 * The smallest sphere around what a light can reach (centre xyz, radius w):
 * a point light's own; a spot's around its cone (with its round cap), much
 * smaller for a narrow one.
 */
glm::vec4 lightBounds(const PointLight& light);

/**
 * Whether a sphere (`center`, `radius`) might be lit by `light`: within its
 * reach, and for a spot inside (or touching) its cone. Conservative: never
 * false for a sphere it reaches.
 */
bool lightReaches(const PointLight& light, glm::vec3 center, float radius);

/**
 * Whether a spot's cone reaches into a shadow cube's face (+x, -x, +y, -y,
 * +z, -z: 0 to 5) around it; always true for a point light. Conservative.
 */
bool lightReachesFace(const PointLight& light, int face);

/**
 * Shadow settings: the sun's shadow map, and point lights' (a cube map each).
 *
 * @param enabled    whether the sun casts shadows at all
 * @param distance   radius (world units) around the camera target that
 *                   receives shadows; larger covers more ground at lower
 *                   detail
 * @param depthBias  depth offset that prevents self-shadowing "acne"
 * @param softness   filter radius in shadow-map texels (0 = hard edges)
 * @param pointLights how many point lights (and glowing objects' lights) may
 *                   cast shadows at once: the ones nearest what the camera
 *                   looks at, of those whose reach is in view (0: none)
 * @param pointResolution each point light's shadow map's face size, in texels
 * @param cascades   1: one shadow map over `distance` around what the camera
 *                   looks at (the default); 2 to 4: cascaded shadow maps along
 *                   the view instead, each covering a farther slice of it at
 *                   less detail, out to `cascadeDistance` (3D games)
 * @param cascadeDistance how far from the camera cascaded shadows reach,
 *                   world units (they fade out over the last stretch)
 * @param cascadeSplit where the view is split between cascades: 1 keeps them
 *                   small near the camera (detail close by), 0 spreads them
 *                   evenly (between, a blend)
 */
struct ShadowSettings
{
  bool enabled = true;
  float distance = 40.0f;
  float depthBias = 0.0015f;
  float softness = 1.5f;
  uint32_t pointLights = 16;
  uint32_t pointResolution = 256;
  uint32_t cascades = 1;
  float cascadeDistance = 200.0f;
  float cascadeSplit = 0.75f;

  bool operator==(const ShadowSettings&) const = default;
};

/**
 * The scene's wind: what grass (GrassComponent) sways in, and anything else
 * that moves with it. Gusts roll across the ground in waves, the way the wind
 * blows.
 *
 * @param direction  which way it blows, in degrees from +X toward +Y
 * @param strength   how hard (0: still air; 1: a gale)
 * @param gustiness  how much it comes and goes in gusts (0: steady)
 */
struct WindSettings
{
  float direction = 30.0f;
  float strength = 0.35f;
  float gustiness = 0.5f;
};

/**
 * Bloom: light brighter than full white (emission above 1, bright glints)
 * spreading a soft glow around it, as a camera sees bright lights. Ordinary
 * lit surfaces never go above 1, so only things that should glow do.
 *
 * @param enabled    whether to bloom at all
 * @param intensity  how strong the glow is
 * @param threshold  brightness (in the brightest channel) above which light
 *                   blooms; 1 is full white
 * @param softness   how gently light starts blooming above the threshold
 *                   (0 = sharply)
 * @param radius     how far the glow spreads (1 = normal)
 */
struct BloomSettings
{
  bool enabled = true;
  float intensity = 0.6f;
  float threshold = 1.0f;
  float softness = 0.5f;
  float radius = 1.0f;

  bool operator==(const BloomSettings&) const = default;
};

/**
 * Depth of field: what's nearer or farther than the camera's focus blurs, as
 * through a real lens. The focus is the view's own (Camera::distance: a
 * camera entity's focusDistance, or the editor's orbit target).
 *
 * The sharp part deepens the farther the focus, as through a real lens (a
 * thin lens's circle of confusion: aperture x |d - s| / d, at the focus s):
 * focused `hyperfocal` away or farther, everything beyond it is sharp, so a
 * far overview of a landscape is in focus but for, at most, its horizon.
 * Close up, where a real lens would leave very little sharp, `range` is
 * always kept sharp.
 *
 * @param enabled     whether to blur at all
 * @param range       how deep the sharp part around the focus is, at least,
 *                    world units
 * @param blur        how blurred the farthest (and nearest) things get (1 =
 *                    normal; 0: none)
 * @param hyperfocal  the lens's hyperfocal distance, world units: focused
 *                    this far, everything beyond is sharp (0: no lens, only
 *                    `range`)
 */
struct DepthOfFieldSettings
{
  bool enabled = false;
  float range = 10.0f;
  float blur = 1.0f;
  float hyperfocal = 80.0f;
};

/**
 * Haze: the air between the camera and far things, fading them into the sky's
 * colour, as distance does outdoors. It hides where things stop being drawn
 * (grass's draw distance, the far plane).
 *
 * @param strength  how hazed the farthest things get (0: none; 1: fully)
 * @param distance  how far away things are half that hazed, world units
 */
struct HazeSettings
{
  float strength = 0.0f;
  float distance = 120.0f;

  bool operator==(const HazeSettings&) const = default;
};

} // namespace sfs
