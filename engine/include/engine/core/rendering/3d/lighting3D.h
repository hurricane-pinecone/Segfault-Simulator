#pragma once

#include "glm/glm/ext/vector_float3.hpp"

#include <cstdint>

namespace sfs
{

/**
 * The directional sun. Same knobs as the isometric path's ambient lighting, so
 * a day/night controller drives either renderer the same way.
 *
 * @param direction       vector pointing toward the sun (need not be unit
 *                        length); Z is up, so Z <= 0 means the sun has set
 * @param color           linear RGB tint of sunlight
 * @param diffuseStrength how much a surface facing the sun brightens over
 *                        ambient (0..1)
 */
struct SunLight3D
{
  glm::vec3 direction{-0.45f, -0.35f, 0.82f};
  glm::vec3 color{1.0f, 0.96f, 0.88f};
  float diffuseStrength = 0.85f;
};

/**
 * Scene-wide lighting: the sun plus the ambient level and sky colour.
 *
 * @param ambient  base brightness everything receives (0 = black night,
 *                 1 = fully lit); also gates point lights, which fade in as
 *                 ambient drops, exactly like the isometric renderer
 * @param skyColor linear RGB background clear colour
 */
struct SceneLighting3D
{
  SunLight3D sun;
  float ambient = 0.55f;
  glm::vec3 skyColor{0.55f, 0.72f, 0.90f};
};

/**
 * One point light, resolved to world space for a frame.
 *
 * @param position  world position
 * @param color     linear RGB tint
 * @param intensity brightness multiplier (0 = off)
 * @param radius    reach in world units; contributes nothing beyond it
 * @param castsShadow whether things block it (if it's among the frame's
 *                  shadowed lights; see ShadowSettings3D::pointLights)
 * @param shadowNear what's within this distance of it (along any axis: a box
 *                  around it) doesn't block it; for a glowing object's light,
 *                  the object's own glowing surface
 * @param sourceRadius how big what gives off the light is (0: a point): shiny
 *                  surfaces reflect a highlight that big, as a sphere light's
 */
struct PointLight3D
{
  glm::vec3 position{0.0f};
  glm::vec3 color{1.0f, 0.85f, 0.6f};
  float intensity = 1.0f;
  float radius = 8.0f;
  bool castsShadow = false;
  float shadowNear = 0.05f;
  float sourceRadius = 0.0f;
};

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
 */
struct ShadowSettings3D
{
  bool enabled = true;
  float distance = 40.0f;
  float depthBias = 0.0015f;
  float softness = 1.5f;
  uint32_t pointLights = 16;
  uint32_t pointResolution = 256;
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
struct WindSettings3D
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
struct BloomSettings3D
{
  bool enabled = true;
  float intensity = 0.6f;
  float threshold = 1.0f;
  float softness = 0.5f;
  float radius = 1.0f;
};

/**
 * Depth of field: what's nearer or farther than the camera's focus blurs, as
 * through a real lens. The focus is the view's own (Camera3D::distance: a
 * camera entity's focusDistance, or the editor's orbit target).
 *
 * @param enabled  whether to blur at all
 * @param range    how deep the sharp part around the focus is, world units
 * @param blur     how blurred the farthest (and nearest) things get (1 =
 *                 normal; 0: none)
 */
struct DepthOfFieldSettings3D
{
  bool enabled = false;
  float range = 10.0f;
  float blur = 1.0f;
};

/**
 * Haze: the air between the camera and far things, fading them into the sky's
 * colour, as distance does outdoors. It hides where things stop being drawn
 * (grass's draw distance, the far plane).
 *
 * @param strength  how hazed the farthest things get (0: none; 1: fully)
 * @param distance  how far away things are half that hazed, world units
 */
struct HazeSettings3D
{
  float strength = 0.0f;
  float distance = 120.0f;
};

} // namespace sfs
