#pragma once

#include "engine/core/rendering/ambientOcclusion.h"
#include "engine/core/rendering/colorGrading.h"
#include "engine/core/rendering/fog.h"
#include "engine/core/rendering/lighting.h"
#include "engine/core/rendering/sky.h"
#include "engine/core/rendering/visualStyle.h"

class EditorHistory;

// Editors for a scene's look: its colour grading (Scene Settings, while no
// visual style is chosen) and a whole visual style (a .style asset, in the
// Inspector). Rows are label-left, widget-right; with a history, each edit
// is one undo step (see widgets::undoable). Each returns true when anything
// changed.
namespace lookWidgets
{

bool grading(EditorHistory* history, sfs::ColorGrading& grading);

// The sky: flat or physical, and (only when physical) the atmosphere's
// settings.
bool sky(EditorHistory* history, sfs::SkySettings& sky);

// The sun's shadow cascades: how many, and (with more than one) how far they
// reach and how they split the view.
bool shadowCascades(EditorHistory* history, sfs::ShadowSettings& shadows);

// Ambient occlusion: on or off, and (only when on) its radius, intensity and
// samples.
bool ambientOcclusion(EditorHistory* history,
                      sfs::AmbientOcclusionSettings& occlusion);

// Height fog: on or off, and (only when on) how thick, where, and its glow.
bool fog(EditorHistory* history, sfs::FogSettings& fog);

bool visualStyle(sfs::VisualStyle& style);

} // namespace lookWidgets
