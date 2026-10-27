#pragma once

#include "camera/viewportCamera.h"
#include "panels/panel.h"

#include "engine/webgpu/webGpuRenderTarget.h"

namespace sfs
{
class WebGpuRenderBackend;
}

// Shows the edited scene: the backend renders the scene into an offscreen
// target sized to this panel, which is drawn here as an image. Mouse input over
// the image navigates the camera.
class ViewportPanel : public Panel
{
public:
  explicit ViewportPanel(sfs::WebGpuRenderBackend& backend);
  ~ViewportPanel() override;

  void update(EditorContext& context) override;

protected:
  void drawContents(EditorContext& context) override;
  int windowFlags() const override;

private:
  void navigate(EditorContext& context, float viewportHeight);

  sfs::WebGpuRenderBackend& m_backend;
  sfs::WebGpuRenderTarget m_target;
  ViewportCamera m_navigation;

  // The panel's image size from the last draw; the target takes it on the next
  // update, before the scene renders into it.
  glm::ivec2 m_desiredSize{0, 0};
};
