#pragma once

#include "glm/glm/ext/matrix_float3x3.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "nlohmann/json.hpp"

#include <optional>
#include <string_view>

namespace sfs
{

/**
 * How the final picture's light is turned into what a screen can show
 * (0..1), in the last pass (after bloom).
 *
 * - None: clamped at white, as the engine always did (lit surfaces never go
 *   above 1, so only glows lose anything). Lighting stays clamped at 1 too.
 * - Aces: the film industry's look (Stephen Hill's fit of the ACES RRT/ODT):
 *   punchy contrast, highlights rolled off, a little warmth.
 * - AgX: Troy Sobotka's filmic curve (as Blender 4 uses): bright colours
 *   desaturate toward white the way film and eyes see them; mid-grey stays.
 * - Neutral: Khronos PBR Neutral: colours as close as can be to the
 *   materials' own, only the brightest compressed.
 *
 * With any but None, lighting is no longer clamped at 1 (the curve rolls
 * highlights off instead), so a bright sun can be brighter than white.
 */
enum class ToneMapper
{
  None,
  Aces,
  AgX,
  Neutral,
};

/** "none", "aces", "agx" or "neutral". */
const char* toneMapperName(ToneMapper mapper);
/** The mapper a toneMapperName() means; nothing for any other name. */
std::optional<ToneMapper> toneMapperFromName(std::string_view name);

/**
 * The last pass's colour grading: tone mapping, exposure, white balance and
 * the colourist's controls. Applied in this order to the scene's light (bloom
 * added): exposure, white balance, contrast and saturation (on the light,
 * before tone mapping), the tone mapper, then lift / gamma / gain and the
 * vignette (on what the screen shows). The defaults change nothing.
 *
 * @param toneMapper  see ToneMapper
 * @param exposure    in stops (EV): +1 doubles the light, -1 halves it
 * @param temperature white balance: -1 cooler (bluer) .. +1 warmer (more
 *                    orange)
 * @param tint        white balance: -1 greener .. +1 more magenta
 * @param contrast    around mid-grey (1: as is; above, punchier)
 * @param saturation  0 grey, 1 as is, above more colourful
 * @param lift        added to the shadows (per channel; 0 as is)
 * @param gamma       bends the midtones (per channel; 1 as is, above
 *                    brighter)
 * @param gain        multiplies the highlights (per channel; 1 as is)
 * @param vignette    how much the corners darken (0: not at all)
 * @param vignetteSoftness how far in from the corners it starts (0: only the
 *                    corners, sharply; 1: from the middle, gently)
 */
struct ColorGrading
{
  ToneMapper toneMapper = ToneMapper::None;
  float exposure = 0.0f;
  float temperature = 0.0f;
  float tint = 0.0f;
  float contrast = 1.0f;
  float saturation = 1.0f;
  glm::vec3 lift{0.0f};
  glm::vec3 gamma{1.0f};
  glm::vec3 gain{1.0f};
  float vignette = 0.0f;
  float vignetteSoftness = 0.5f;

  /** Whether it changes nothing: the picture exactly as with no grading. */
  bool neutral() const;

  nlohmann::json toJson() const;
  /** Missing keys keep `defaults`' values (or the defaults). */
  static ColorGrading fromJson(const nlohmann::json& json,
                               const ColorGrading& defaults);
  static ColorGrading fromJson(const nlohmann::json& json);

  bool operator==(const ColorGrading&) const = default;
};

/**
 * The white balance as a matrix on linear RGB: the light's colour shifted by
 * `temperature` and `tint` (see ColorGrading), as a camera's white balance
 * does it (in LMS cone space, from the D65 white point). Identity at 0, 0.
 */
glm::mat3 whiteBalanceMatrix(float temperature, float tint);

/**
 * How a visual style re-balances a scene's light (on top of what the scene,
 * or its day/night cycle, sets): the sun's share of each surface's light and
 * the ambient (sky) share, each scaled per channel. Neutral: as the scene has
 * it.
 */
struct LightBalance
{
  glm::vec3 sun{1.0f};
  glm::vec3 ambient{1.0f};

  bool neutral() const { return *this == LightBalance{}; }
  bool operator==(const LightBalance&) const = default;
};

} // namespace sfs
