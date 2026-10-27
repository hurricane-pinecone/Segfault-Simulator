#pragma once

#include "panels/panel.h"

// Edits the selected entity's components. The editors here are hand-written
// for the engine's built-in components; reflection (generated from component
// declarations) will replace them so every component, including a game's own,
// gets an editor automatically.
class InspectorPanel : public Panel
{
public:
  InspectorPanel() : Panel("Inspector") {}

protected:
  void drawContents(EditorContext& context) override;
};
