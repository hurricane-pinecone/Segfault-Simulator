#pragma once

#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "nlohmann/json.hpp"

#include <cstdint>

namespace sfs
{

/**
 * How rain looks (the weather says how hard it's raining: SceneWeather::rain,
 * and how wet things are: SceneWeather::wetness). A 3D scene's, and a visual
 * style's to override.
 *
 * While it rains, streaks fall through the air around the camera (slanted by
 * the weather's wind, lit by the sky and lamps, faded with distance), drops
 * splash where they land and ring the water they fall on, and the distance
 * greys over behind a veil of rain. Nothing falls (or splashes) under a roof:
 * an overhead map of what's drawn says where the sky is open (with the sun's
 * shadows on; without them, it rains everywhere and nothing splashes).
 *
 * @param enabled    whether rain is drawn at all (wet surfaces still are)
 * @param density    how many streaks fall (1: about 40,000 in a downpour;
 *                   far fewer in a drizzle: they grow faster than the rain)
 * @param length     how long a streak is, metres (its blur as it falls)
 * @param width      how wide it is, millimetres (at least a pixel is drawn,
 *                   fainter)
 * @param brightness how bright the streaks are (1: as the sky lights them)
 * @param splashes   how many drops splash where they land (0: none; 1:
 *                   about 8,000 at once in a downpour)
 * @param ripples    how strongly drops ring standing water (0: none)
 * @param veil       how thick the distant rain's grey veil is (0: none; 1:
 *                   a downpour hides about half of what's 150 m away)
 * @param drips      how much water drips off the edges of roofs, eaves and
 *                   canopies in heavy rain, and a while after (0: none; 1:
 *                   about 4,000 places looked for a drip around the
 *                   camera), and how wet the ground is under them
 */
struct RainSettings
{
  bool enabled = true;
  float density = 1.0f;
  float length = 0.5f;
  float width = 2.0f;
  float brightness = 1.0f;
  float splashes = 1.0f;
  float ripples = 1.0f;
  float veil = 1.0f;
  float drips = 1.0f;

  nlohmann::json toJson() const;
  /** Missing keys keep `defaults`' values. */
  static RainSettings fromJson(const nlohmann::json& json,
                               const RainSettings& defaults);
  static RainSettings fromJson(const nlohmann::json& json);

  bool operator==(const RainSettings&) const = default;
};

/** The most streaks drawn (density 1, rain 1). */
inline constexpr uint32_t kRainStreaks = 40000;
/** The most splashes drawn at once (splashes 1, rain 1). */
inline constexpr uint32_t kRainSplashes = 8000;

/** The most places drips are looked for (drips 1, a downpour). */
inline constexpr uint32_t kRainDrips = 4000;

/**
 * Water dripping off edges (RainSettings::drips): where something on rain's
 * overhead map stands well above the open ground beside it (an eave, an
 * awning, a canopy's rim), drops gather and fall from it, and splash where
 * they land. Heavy rain only (none below about 0.4), and fewer and slower
 * for a while after it stops, while things are still soaked.
 *
 * @param count    how many places drips are looked for (rain.wgsl draws one
 *                 drip at a time from each it finds an edge for)
 * @param rate     the share of those places that drip each round (about
 *                 1.9 s: a drop gathering, falling and splashing): all of
 *                 them in a downpour, fewer after the rain
 * @param trickle  how much the drops string out into broken trickles
 *                 (0: single drops; 1: a downpour's streams)
 * @param line     how wet the ground is along the drip line, under an
 *                 edge (0..1)
 */
struct RainDrips
{
  uint32_t count = 0;
  float rate = 0.0f;
  float trickle = 0.0f;
  float line = 0.0f;
};

/** The drips in `rain` (0..1) once surfaces are `wetness` (0..1) wet. */
RainDrips rainDrips(const RainSettings& settings, float rain, float wetness);

/** How many streaks fall in `rain` (0..1) of these settings. */
uint32_t rainStreaks(const RainSettings& settings, float rain);
/** How many drops splash at once in `rain` (0..1). */
uint32_t rainSplashes(const RainSettings& settings, float rain);
/**
 * How much of the light a metre of the veil takes (its density, as height
 * fog's but the same at every height) in `rain` (0..1).
 */
float rainVeilDensity(const RainSettings& settings, float rain);

/**
 * The overhead map of what's drawn rain falls past: depth looking straight
 * down over a square ahead of the camera (it holds the ground the view
 * mostly sees), so whatever has something above it is sheltered.
 *
 * @param center     the square's middle (world XY), snapped to a whole
 *                   number of the map's texels so it doesn't shimmer
 * @param top        the height its depth starts at (0) ...
 * @param depthRange ... and how far down it reaches (1)
 * @param halfSize   half the square's width
 * @param viewProjection world -> its clip space (z 0 at the top)
 */
struct RainMap
{
  glm::vec2 center{0.0f};
  float top = 0.0f;
  float depthRange = 1.0f;
  float halfSize = 1.0f;
  glm::mat4 viewProjection{1.0f};
};

/** The overhead map's resolution (texels across). */
inline constexpr uint32_t kRainMapResolution = 2048;
/** Half its width, metres (6 cm a texel). */
inline constexpr float kRainMapHalfSize = 64.0f;

/** The overhead map for a camera at `camera` looking along `forward`. */
RainMap rainMapFor(const glm::vec3& camera, const glm::vec3& forward);

/**
 * How far rain's drops drift across the world (metres) before their pattern
 * repeats: a whole number of both boxes of drops rain.wgsl draws (24 and 70
 * m wide), so the drift is kept within it without a jump.
 */
inline constexpr float kRainDriftPeriod = 840.0f;

/**
 * Rain's drift on the wind, eased so a gust or a veer turns the drops
 * smoothly rather than flicking them sideways: `wind` (the drops' wind,
 * m/s) eased toward `target` over about a second, and `drift` (m, within
 * kRainDriftPeriod) moved on by it, after `seconds` more. The drops fall
 * slower sideways than the wind (0.8 of it).
 */
struct RainDrift
{
  glm::vec2 drift{0.0f};
  glm::vec2 wind{0.0f};
  bool started = false; // eased from the first wind it's given

  void advance(const glm::vec2& target, float seconds);
};

} // namespace sfs
