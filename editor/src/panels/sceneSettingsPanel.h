#pragma once

#include "engine/core/events/eventQueue.h"
#include "panels/panel.h"

#include <vector>

// Scene-wide settings: lighting, shadows and camera projection, drawn from the
// render system's ModuleSetting descriptors (the same ones the in-game debug
// panel shows), plus last frame's render stats.
class SceneSettingsPanel : public Panel
{
public:
  SceneSettingsPanel() : Panel("Scene Settings") {}

protected:
  void drawContents(EditorContext& context) override;

private:
  // The Events view's rows, refilled each frame: kept so they allocate
  // nothing once they have room.
  std::vector<sfs::EventStats> m_eventStats;
  std::vector<sfs::EventConsumerStats> m_eventConsumers;
};
