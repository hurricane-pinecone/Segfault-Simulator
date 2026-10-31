#include "widgets/lookWidgets.h"

#include "widgets/widgets.h"

#include "imgui.h"

#include <algorithm>
#include <iterator>

namespace lookWidgets
{

namespace
{

// A row: the label, then the widget (by a hidden id) filling the rest.
std::string row(const char* label)
{
  widgets::propertyLabel(label);
  ImGui::SetNextItemWidth(-FLT_MIN);
  return std::string("##") + label;
}

bool slider(EditorHistory* history,
            const char* label,
            float& value,
            float min,
            float max,
            const char* tooltip,
            const char* format = "%.2f")
{
  const bool changed = widgets::undoable(
      history,
      value,
      [&](float& v)
      { return ImGui::SliderFloat(row(label).c_str(), &v, min, max, format); });
  ImGui::SetItemTooltip("%s", tooltip);
  return changed;
}

bool drag3(EditorHistory* history,
           const char* label,
           glm::vec3& value,
           float speed,
           float min,
           float max,
           const char* tooltip)
{
  const bool changed = widgets::undoable(
      history,
      value,
      [&](glm::vec3& v)
      {
        return ImGui::DragFloat3(
            row(label).c_str(), &v.x, speed, min, max, "%.3f");
      });
  ImGui::SetItemTooltip("%s", tooltip);
  return changed;
}

bool color(EditorHistory* history,
           const char* label,
           glm::vec3& value,
           const char* tooltip)
{
  const bool changed = widgets::undoable(
      history,
      value,
      [&](glm::vec3& v)
      { return widgets::colorEdit3(row(label).c_str(), &v.x); });
  ImGui::SetItemTooltip("%s", tooltip);
  return changed;
}

} // namespace

bool grading(EditorHistory* history, sfs::ColorGrading& grading)
{
  bool changed = false;
  const char* mappers[] = {"None (clamp)", "ACES", "AgX", "Neutral"};
  changed |= widgets::undoable(history,
                               grading.toneMapper,
                               [&](sfs::ToneMapper& mapper)
                               {
                                 int index = static_cast<int>(mapper);
                                 const bool picked = ImGui::Combo(
                                     row("Tone Mapping").c_str(),
                                     &index,
                                     mappers,
                                     static_cast<int>(std::size(mappers)));
                                 if (picked)
                                   mapper = static_cast<sfs::ToneMapper>(index);
                                 return picked;
                               });
  ImGui::SetItemTooltip(
      "How light is turned into what the screen shows.\n"
      "None: clamped at white (the engine's classic look; lighting stays "
      "clamped too).\n"
      "ACES: filmic contrast, highlights rolled off.\n"
      "AgX: filmic, bright colours fade toward white as on film.\n"
      "Neutral: the materials' colours kept, only the brightest compressed.\n"
      "With any but None, lighting may go past white.");
  changed |= slider(history,
                    "Exposure",
                    grading.exposure,
                    -4.0f,
                    4.0f,
                    "In stops: +1 doubles the light, -1 halves it",
                    "%+.2f EV");
  changed |= slider(history,
                    "Temperature",
                    grading.temperature,
                    -1.0f,
                    1.0f,
                    "White balance: cooler (bluer) to warmer (more orange)");
  changed |= slider(history,
                    "Tint",
                    grading.tint,
                    -1.0f,
                    1.0f,
                    "White balance: greener to more magenta");
  changed |= slider(history,
                    "Contrast",
                    grading.contrast,
                    0.0f,
                    2.0f,
                    "Around mid-grey: 1 as is, above punchier");
  changed |= slider(history,
                    "Saturation",
                    grading.saturation,
                    0.0f,
                    2.0f,
                    "0 grey, 1 as is, above more colourful");
  changed |= drag3(history,
                   "Lift",
                   grading.lift,
                   0.002f,
                   -0.5f,
                   0.5f,
                   "Added to the shadows, per channel (0: as is)");
  changed |=
      drag3(history,
            "Gamma",
            grading.gamma,
            0.005f,
            0.2f,
            3.0f,
            "Bends the midtones, per channel (1: as is; above, brighter)");
  changed |= drag3(history,
                   "Gain",
                   grading.gain,
                   0.005f,
                   0.0f,
                   3.0f,
                   "Multiplies the highlights, per channel (1: as is)");
  changed |= slider(history,
                    "Vignette",
                    grading.vignette,
                    0.0f,
                    1.0f,
                    "How much the corners darken");
  if (grading.vignette > 0.0f)
    changed |= slider(history,
                      "Vignette Softness",
                      grading.vignetteSoftness,
                      0.0f,
                      1.0f,
                      "How far in from the corners it starts");
  return changed;
}

bool sky(EditorHistory* history, sfs::SkySettings& sky)
{
  bool changed = widgets::undoable(
      history,
      sky.model,
      [&](sfs::SkyModel& model)
      {
        const char* models[] = {"Flat (the sky colour)", "Physical"};
        int index = static_cast<int>(model);
        const bool picked =
            ImGui::Combo(row("Sky").c_str(), &index, models, 2);
        if (picked)
          model = static_cast<sfs::SkyModel>(index);
        return picked;
      });
  ImGui::SetItemTooltip(
      "Flat: the sky colour behind everything, and the classic lighting.\n"
      "Physical: an atmosphere lit by the sun (blue by day, warm at sunrise\n"
      "and sunset, dark at night) that lights the scene itself: the sun's\n"
      "colour through the air, the sky's light all around, and its\n"
      "reflections. Only the sun's direction comes from the scene. Best with\n"
      "a tone mapper (Grading).");
  if (!sky.physical())
    return changed;
  changed |= slider(history,
                    "Turbidity",
                    sky.turbidity,
                    1.0f,
                    10.0f,
                    "How hazy the air is: 1 very clear, 2 a clear day, 4-6 "
                    "hazy, 10 thick haze");
  changed |= slider(history,
                    "Ground Albedo",
                    sky.groundAlbedo,
                    0.0f,
                    1.0f,
                    "How much light the ground reflects: below the horizon, "
                    "and the light it bounces up");
  changed |= slider(history,
                    "Sun Size",
                    sky.sunSize,
                    0.0f,
                    5.0f,
                    "The sun's disk, x the real sun's (0: none drawn)");
  changed |= slider(history,
                    "Intensity",
                    sky.intensity,
                    0.0f,
                    4.0f,
                    "How bright the sun and sky are, x");
  changed |= slider(history,
                    "Night Brightness",
                    sky.nightBrightness,
                    0.0f,
                    4.0f,
                    "The night sky's glow (moon- and starlight), and its "
                    "stars, x; 0 a black night");
  changed |= slider(history,
                    "Adaptation",
                    sky.adaptation,
                    0.0f,
                    1.0f,
                    "How much eyes adapt as the sky dims (dusk, night): 0 a "
                    "fixed exposure,\n1 fully (up to 3 stops). It brightens "
                    "the sky's light, not lamps or glowing things");
  return changed;
}

bool shadowCascades(EditorHistory* history, sfs::ShadowSettings& shadows)
{
  bool changed = widgets::undoable(
      history,
      shadows.cascades,
      [&](uint32_t& cascades)
      {
        int count = static_cast<int>(cascades);
        const bool moved =
            ImGui::SliderInt(row("Shadow Cascades").c_str(), &count, 1, 4);
        if (moved)
          cascades = static_cast<uint32_t>(std::clamp(count, 1, 4));
        return moved;
      });
  ImGui::SetItemTooltip(
      "1: one shadow map around what the camera looks at (Shadow distance).\n"
      "2-4: cascaded shadow maps along the view, finer near the camera,\n"
      "reaching Cascade Distance away (3D scenes).");
  if (shadows.cascades <= 1)
    return changed;
  changed |= slider(history,
                    "Cascade Distance",
                    shadows.cascadeDistance,
                    20.0f,
                    1000.0f,
                    "How far from the camera the sun's shadows reach",
                    "%.0f");
  changed |= slider(history,
                    "Cascade Split",
                    shadows.cascadeSplit,
                    0.0f,
                    1.0f,
                    "Where the view is split between cascades: 1 keeps them "
                    "small near the camera (detail close by), 0 even");
  return changed;
}

bool ambientOcclusion(EditorHistory* history,
                      sfs::AmbientOcclusionSettings& occlusion)
{
  bool changed = widgets::undoable(
      history,
      occlusion.enabled,
      [&](bool& on)
      { return ImGui::Checkbox(row("Ambient Occlusion").c_str(), &on); });
  ImGui::SetItemTooltip(
      "Keeps the light from all around (the sky, the ambient level) out of\n"
      "creases, corners and the ground at the foot of walls. Worked out on\n"
      "screen at half resolution; never darkens the sun's or lamps' light.\n"
      "Grass blades don't occlude.");
  if (!occlusion.enabled)
    return changed;
  changed |= slider(history,
                    "Radius",
                    occlusion.radius,
                    0.1f,
                    5.0f,
                    "How far around a point it looks for what blocks it, "
                    "world units");
  changed |= slider(history,
                    "Intensity",
                    occlusion.intensity,
                    0.0f,
                    4.0f,
                    "How dark it gets (1 normal)");
  changed |= widgets::undoable(
      history,
      occlusion.samples,
      [&](uint32_t& samples)
      {
        int count = static_cast<int>(samples);
        const bool moved = ImGui::SliderInt(
            row("Samples").c_str(),
            &count,
            static_cast<int>(sfs::kMinOcclusionSamples),
            static_cast<int>(sfs::kMaxOcclusionSamples));
        if (moved)
          samples = static_cast<uint32_t>(count);
        return moved;
      });
  ImGui::SetItemTooltip(
      "Points looked at around each pixel: more is smoother, and costs more");
  return changed;
}

bool contactShadows(EditorHistory* history, sfs::ContactShadowSettings& contact)
{
  bool changed = widgets::undoable(
      history,
      contact.enabled,
      [&](bool& on)
      { return ImGui::Checkbox(row("Contact Shadows").c_str(), &on); });
  ImGui::SetItemTooltip(
      "Small, sharp shadows where things meet the ground and in fine\n"
      "crevices, finer than the sun's shadow maps hold. Worked out on screen\n"
      "with ambient occlusion; take only the sun's (or moon's) light, under a\n"
      "physical sky. Grass blades don't cast them.");
  if (!contact.enabled)
    return changed;
  changed |= slider(history,
                    "Length",
                    contact.length,
                    0.05f,
                    2.0f,
                    "How far toward the sun it looks for what blocks it, "
                    "world units");
  changed |= slider(history,
                    "Thickness",
                    contact.thickness,
                    0.02f,
                    2.0f,
                    "How thick what's drawn is taken to be behind its front, "
                    "world units (more: thin things cast more broadly)");
  changed |= slider(history,
                    "Strength",
                    contact.strength,
                    0.0f,
                    1.0f,
                    "How dark they get (1: none of the sun's light)");
  return changed;
}

bool fog(EditorHistory* history, sfs::FogSettings& fog)
{
  bool changed = widgets::undoable(
      history,
      fog.enabled,
      [&](bool& on) { return ImGui::Checkbox(row("Height Fog").c_str(), &on); });
  ImGui::SetItemTooltip(
      "Air thicker low down, as morning mist: far things fade into the sky's\n"
      "colour toward where you look (glowing toward the sun, under a\n"
      "physical sky). Takes haze's place while on.");
  if (!fog.enabled)
    return changed;
  changed |= slider(history,
                    "Density",
                    fog.density,
                    0.0f,
                    0.2f,
                    "How thick it is at its base height (0.02: things 50 m "
                    "away about two-thirds hidden)",
                    "%.4f");
  changed |= slider(history,
                    "Height Falloff",
                    fog.heightFalloff,
                    0.0f,
                    1.0f,
                    "How quickly it thins going up, per metre (0: the same "
                    "at every height)",
                    "%.3f");
  changed |= slider(history,
                    "Base Height",
                    fog.baseHeight,
                    -50.0f,
                    100.0f,
                    "The height (world Z) it's that thick at",
                    "%.1f");
  changed |= slider(history,
                    "Start Distance",
                    fog.startDistance,
                    0.0f,
                    300.0f,
                    "How far from the camera it starts",
                    "%.0f");
  changed |= slider(history,
                    "Max Opacity",
                    fog.maxOpacity,
                    0.0f,
                    1.0f,
                    "The most it hides anything, however far");
  changed |= slider(history,
                    "Sun Glow",
                    fog.sunGlow,
                    0.0f,
                    2.0f,
                    "How much it glows toward the sun (a physical sky's)");
  return changed;
}

bool visualStyle(sfs::VisualStyle& style)
{
  bool changed = false;

  ImGui::SeparatorText("Grading");
  changed |= grading(nullptr, style.grading);

  ImGui::SeparatorText("Light");
  ImGui::TextDisabled("Multiplies the scene's (and its day/night cycle's)");
  changed |= slider(nullptr,
                    "Sun Intensity",
                    style.sunIntensity,
                    0.0f,
                    4.0f,
                    "The sun's share of each surface's light: what shadows "
                    "and turning away take");
  changed |= color(nullptr, "Sun Tint", style.sunTint, "The sun's colour, x");
  changed |= slider(nullptr,
                    "Ambient Intensity",
                    style.ambientIntensity,
                    0.0f,
                    4.0f,
                    "The light from all around (the sky): what's left in "
                    "shadow");
  changed |=
      color(nullptr, "Ambient Tint", style.ambientTint, "Ambient's colour, x");
  changed |= slider(nullptr,
                    "Sky Intensity",
                    style.skyIntensity,
                    0.0f,
                    4.0f,
                    "The sky colour: the background, the haze, what shiny "
                    "things reflect");
  changed |= color(nullptr, "Sky Tint", style.skyTint, "The sky's colour, x");

  ImGui::SeparatorText("Replaces The Scene's");
  // Each optional: on, the style's value stands in for the scene's.
  const auto optional = [&](const char* label, bool on, const char* tooltip)
  {
    bool value = on;
    const bool toggled = widgets::propertyCheckbox(label, &value);
    ImGui::SetItemTooltip("%s", tooltip);
    return toggled;
  };
  if (optional("Bloom",
               style.bloom.has_value(),
               "On: this bloom instead of the scene camera's"))
  {
    style.bloom = style.bloom
                      ? std::nullopt
                      : std::optional<sfs::BloomSettings>(sfs::BloomSettings{});
    changed = true;
  }
  if (style.bloom)
  {
    ImGui::Indent();
    ImGui::PushID("bloom");
    sfs::BloomSettings& bloom = *style.bloom;
    changed |= widgets::propertyCheckbox("Glow", &bloom.enabled);
    changed |=
        slider(nullptr, "Intensity", bloom.intensity, 0.0f, 3.0f, "How strong");
    changed |= slider(nullptr,
                      "Threshold",
                      bloom.threshold,
                      0.0f,
                      4.0f,
                      "Light above this blooms (1: full white)");
    changed |= slider(nullptr,
                      "Softness",
                      bloom.softness,
                      0.0f,
                      2.0f,
                      "How gently it starts above the threshold");
    changed |= slider(
        nullptr, "Radius", bloom.radius, 0.1f, 3.0f, "How far it spreads");
    ImGui::PopID();
    ImGui::Unindent();
  }
  if (optional("Haze",
               style.haze.has_value(),
               "On: this haze instead of the scene's"))
  {
    style.haze =
        style.haze
            ? std::nullopt
            : std::optional<sfs::HazeSettings>(sfs::HazeSettings{0.2f, 120.0f});
    changed = true;
  }
  if (style.haze)
  {
    ImGui::Indent();
    changed |= slider(nullptr,
                      "Haze Strength",
                      style.haze->strength,
                      0.0f,
                      1.0f,
                      "How hazed the farthest things get");
    changed |= slider(nullptr,
                      "Haze Distance",
                      style.haze->distance,
                      5.0f,
                      1000.0f,
                      "How far away things are half that hazed",
                      "%.0f");
    ImGui::Unindent();
  }
  if (optional("Ambient Occlusion",
               style.ambientOcclusion.has_value(),
               "On: this ambient occlusion instead of the scene's"))
  {
    style.ambientOcclusion =
        style.ambientOcclusion
            ? std::nullopt
            : std::optional<sfs::AmbientOcclusionSettings>(
                  sfs::AmbientOcclusionSettings{.enabled = true});
    changed = true;
  }
  if (style.ambientOcclusion)
  {
    ImGui::Indent();
    ImGui::PushID("occlusion");
    changed |= ambientOcclusion(nullptr, *style.ambientOcclusion);
    ImGui::PopID();
    ImGui::Unindent();
  }
  if (optional("Contact Shadows",
               style.contactShadows.has_value(),
               "On: these contact shadows instead of the scene's"))
  {
    style.contactShadows =
        style.contactShadows ? std::nullopt
                             : std::optional<sfs::ContactShadowSettings>(
                                   sfs::ContactShadowSettings{.enabled = true});
    changed = true;
  }
  if (style.contactShadows)
  {
    ImGui::Indent();
    ImGui::PushID("contact");
    changed |= contactShadows(nullptr, *style.contactShadows);
    ImGui::PopID();
    ImGui::Unindent();
  }
  if (optional("Fog",
               style.fog.has_value(),
               "On: this height fog instead of the scene's (it takes haze's "
               "place while it's on)"))
  {
    style.fog = style.fog ? std::nullopt
                          : std::optional<sfs::FogSettings>(
                                sfs::FogSettings{.enabled = true});
    changed = true;
  }
  if (style.fog)
  {
    ImGui::Indent();
    ImGui::PushID("fog");
    changed |= fog(nullptr, *style.fog);
    ImGui::PopID();
    ImGui::Unindent();
  }
  if (optional("Shadow Softness",
               style.shadowSoftness.has_value(),
               "On: the sun's shadows this soft instead of the scene's"))
  {
    style.shadowSoftness =
        style.shadowSoftness ? std::nullopt : std::optional<float>(1.5f);
    changed = true;
  }
  if (style.shadowSoftness)
  {
    ImGui::Indent();
    ImGui::PushID("shadows");
    changed |= slider(nullptr,
                      "Softness",
                      *style.shadowSoftness,
                      0.0f,
                      4.0f,
                      "Filter radius in shadow-map texels (0: hard edges)");
    ImGui::PopID();
    ImGui::Unindent();
  }

  if (optional("Shadow Cascades",
               style.shadowCascades.has_value(),
               "On: this many shadow cascades instead of the scene's"))
  {
    style.shadowCascades =
        style.shadowCascades ? std::nullopt : std::optional<uint32_t>(4u);
    changed = true;
  }
  if (style.shadowCascades)
  {
    ImGui::Indent();
    ImGui::PushID("cascades");
    // The count, then (with more than one) their reach and split, each
    // optional in turn.
    sfs::ShadowSettings shadows;
    shadows.cascades = *style.shadowCascades;
    shadows.cascadeDistance =
        style.shadowCascadeDistance.value_or(shadows.cascadeDistance);
    shadows.cascadeSplit =
        style.shadowCascadeSplit.value_or(shadows.cascadeSplit);
    int count = static_cast<int>(shadows.cascades);
    if (ImGui::SliderInt(row("Cascades").c_str(), &count, 1, 4))
    {
      style.shadowCascades = static_cast<uint32_t>(std::clamp(count, 1, 4));
      changed = true;
    }
    ImGui::SetItemTooltip("1: one map around what the camera looks at; 2-4: "
                          "cascades along the view");
    if (*style.shadowCascades > 1)
    {
      if (optional("Cascade Distance",
                   style.shadowCascadeDistance.has_value(),
                   "On: the cascades reach this far instead of the scene's"))
      {
        style.shadowCascadeDistance =
            style.shadowCascadeDistance
                ? std::nullopt
                : std::optional<float>(shadows.cascadeDistance);
        changed = true;
      }
      if (style.shadowCascadeDistance)
        changed |= slider(nullptr,
                          "Distance",
                          *style.shadowCascadeDistance,
                          20.0f,
                          1000.0f,
                          "How far from the camera the sun's shadows reach",
                          "%.0f");
      if (optional("Cascade Split",
                   style.shadowCascadeSplit.has_value(),
                   "On: this split instead of the scene's"))
      {
        style.shadowCascadeSplit =
            style.shadowCascadeSplit
                ? std::nullopt
                : std::optional<float>(shadows.cascadeSplit);
        changed = true;
      }
      if (style.shadowCascadeSplit)
        changed |= slider(nullptr,
                          "Split",
                          *style.shadowCascadeSplit,
                          0.0f,
                          1.0f,
                          "1: small near the camera (detail close by); 0: "
                          "even");
    }
    ImGui::PopID();
    ImGui::Unindent();
  }
  if (optional("Sky",
               style.sky.has_value(),
               "On: this sky instead of the scene's (e.g. a physical one)"))
  {
    style.sky = style.sky ? std::nullopt
                          : std::optional<sfs::SkySettings>(sfs::SkySettings{
                                .model = sfs::SkyModel::Physical});
    changed = true;
  }
  if (style.sky)
  {
    ImGui::Indent();
    ImGui::PushID("sky");
    changed |= sky(nullptr, *style.sky);
    ImGui::PopID();
    ImGui::Unindent();
  }

  ImGui::SeparatorText("Materials");
  widgets::propertyLabel("Material Variant");
  ImGui::SetNextItemWidth(-FLT_MIN);
  changed |= widgets::inputText("##variant", style.materialVariant);
  ImGui::SetItemTooltip(
      "Materials with a variant under this tag (e.g. \"realistic\") draw as "
      "it\nwhile this style is on; empty: every material as itself");
  return changed;
}

} // namespace lookWidgets
