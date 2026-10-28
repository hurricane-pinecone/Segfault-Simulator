#pragma once

#include "panels/panel.h"

// Scene-wide settings: lighting, shadows and camera projection, drawn from the
// render system's ModuleSetting descriptors (the same ones the in-game debug
// panel shows), plus last frame's render stats.
class SceneSettingsPanel : public Panel
{
public:
  SceneSettingsPanel() : Panel("Scene Settings") {}

protected:
  void drawContents(EditorContext& context) override;
};
