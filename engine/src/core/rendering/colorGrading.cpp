#include "engine/core/rendering/colorGrading.h"

#include "glm/glm/matrix.hpp"

#include <array>

namespace sfs
{

// Out of the class: a default ColorGrading built inside its own definition
// trips some Clang versions (its member initializers aren't parsed yet).
bool ColorGrading::neutral() const { return *this == ColorGrading{}; }

namespace
{

template <typename T>
void read(const nlohmann::json& json, const char* key, T& out)
{
  if (const auto it = json.find(key); it != json.end() && it->is_number())
    out = it->get<T>();
}

void readVec3(const nlohmann::json& json, const char* key, glm::vec3& out)
{
  if (const auto it = json.find(key);
      it != json.end() && it->is_array() && it->size() == 3)
    for (int i = 0; i < 3; ++i)
      if ((*it)[i].is_number())
        out[i] = (*it)[i].get<float>();
}

nlohmann::json vec3Json(const glm::vec3& v) { return {v.x, v.y, v.z}; }

// A 3x3 matrix from its rows (as papers write them).
glm::mat3 fromRows(const std::array<float, 9>& m)
{
  return glm::transpose(
      glm::mat3(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]));
}

} // namespace

const char* toneMapperName(ToneMapper mapper)
{
  switch (mapper)
  {
  case ToneMapper::Aces:
    return "aces";
  case ToneMapper::AgX:
    return "agx";
  case ToneMapper::Neutral:
    return "neutral";
  case ToneMapper::None:
    break;
  }
  return "none";
}

std::optional<ToneMapper> toneMapperFromName(std::string_view name)
{
  for (const ToneMapper mapper : {ToneMapper::None,
                                  ToneMapper::Aces,
                                  ToneMapper::AgX,
                                  ToneMapper::Neutral})
    if (name == toneMapperName(mapper))
      return mapper;
  return std::nullopt;
}

nlohmann::json ColorGrading::toJson() const
{
  return {
      {"toneMapper", toneMapperName(toneMapper)},
      {"exposure", exposure},
      {"temperature", temperature},
      {"tint", tint},
      {"contrast", contrast},
      {"saturation", saturation},
      {"lift", vec3Json(lift)},
      {"gamma", vec3Json(gamma)},
      {"gain", vec3Json(gain)},
      {"vignette", vignette},
      {"vignetteSoftness", vignetteSoftness},
  };
}

ColorGrading ColorGrading::fromJson(const nlohmann::json& json)
{
  return fromJson(json, ColorGrading{});
}

ColorGrading ColorGrading::fromJson(const nlohmann::json& json,
                                    const ColorGrading& defaults)
{
  ColorGrading out = defaults;
  if (!json.is_object())
    return out;
  if (const auto it = json.find("toneMapper");
      it != json.end() && it->is_string())
    out.toneMapper =
        toneMapperFromName(it->get<std::string>()).value_or(out.toneMapper);
  read(json, "exposure", out.exposure);
  read(json, "temperature", out.temperature);
  read(json, "tint", out.tint);
  read(json, "contrast", out.contrast);
  read(json, "saturation", out.saturation);
  readVec3(json, "lift", out.lift);
  readVec3(json, "gamma", out.gamma);
  readVec3(json, "gain", out.gain);
  read(json, "vignette", out.vignette);
  read(json, "vignetteSoftness", out.vignetteSoftness);
  return out;
}

glm::mat3 whiteBalanceMatrix(float temperature, float tint)
{
  if (temperature == 0.0f && tint == 0.0f)
    return glm::mat3(1.0f);

  // As Unity's post-processing does it: the white point moved along the
  // daylight locus (temperature) and across it (tint), from D65, and the
  // light adapted to it in LMS space (von Kries).
  const float t1 = temperature * 100.0f / 65.0f;
  const float t2 = tint * 100.0f / 65.0f;
  const float x = 0.31271f - t1 * (t1 < 0.0f ? 0.1f : 0.05f);
  const float standardY = 2.87f * x - 3.0f * x * x - 0.27509507f;
  const float y = standardY + t2 * 0.05f;

  // CIE xy (Y = 1) to LMS.
  const float bigX = x / y;
  const float bigZ = (1.0f - x - y) / y;
  const glm::vec3 lms(0.7328f * bigX + 0.4296f - 0.1624f * bigZ,
                      -0.7036f * bigX + 1.6975f + 0.0061f * bigZ,
                      0.0030f * bigX + 0.0136f + 0.9834f * bigZ);
  const glm::vec3 d65(0.949237f, 1.03542f, 1.08728f);
  const glm::vec3 scale = d65 / lms;

  const glm::mat3 toLms = fromRows({3.90405e-1f,
                                    5.49941e-1f,
                                    8.92632e-3f,
                                    7.08416e-2f,
                                    9.63172e-1f,
                                    1.35775e-3f,
                                    2.31082e-2f,
                                    1.28021e-1f,
                                    9.36245e-1f});
  const glm::mat3 fromLms = fromRows({2.85847e+0f,
                                      -1.62879e+0f,
                                      -2.48910e-2f,
                                      -2.10182e-1f,
                                      1.15820e+0f,
                                      3.24281e-4f,
                                      -4.18120e-2f,
                                      -1.18169e-1f,
                                      1.06867e+0f});
  glm::mat3 diagonal(1.0f);
  diagonal[0][0] = scale.x;
  diagonal[1][1] = scale.y;
  diagonal[2][2] = scale.z;
  return fromLms * diagonal * toLms;
}

} // namespace sfs
