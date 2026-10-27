#include "panels/sceneSettingsPanel.h"

#include "editorContext.h"

#include "engine/runtime/rendering/debug/ui.h"
#include "engine/runtime/rendering3d/renderSystem3D.h"

#include "imgui.h"

void SceneSettingsPanel::drawContents(EditorContext& context)
{
  if (!context.render)
    return;

  sfs::RenderSystem3D& render = *context.render;

  // Sun direction and colours aren't ModuleSettings (they're vectors), so they
  // get their own widgets; the rest comes from the descriptors.
  sfs::SceneLighting3D lighting = render.lighting();
  bool lightingChanged = false;
  lightingChanged |= ImGui::DragFloat3("Sun Direction", &lighting.sun.direction.x, 0.01f);
  lightingChanged |= ImGui::ColorEdit3("Sun Color", &lighting.sun.color.x);
  lightingChanged |= ImGui::ColorEdit3("Sky Color", &lighting.skyColor.x);
  if (lightingChanged)
    render.setLighting(lighting);

  for (const sfs::ModuleSetting& setting : render.settings())
    sfs::renderModuleSetting(setting);

  ImGui::Separator();
  const sfs::RenderStats3D& stats = render.stats();
  ImGui::Text("Draw calls: %zu", stats.drawCalls);
  ImGui::Text("Objects: %zu (%zu casting shadows)", stats.instances, stats.shadowInstances);
  ImGui::Text("Point lights: %zu", stats.pointLights);
}
