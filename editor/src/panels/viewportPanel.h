#pragma once

#include "camera/viewportCamera.h"
#include "panels/panel.h"

#include "engine/webgpu/webGpuRenderTarget.h"
#include "glm/glm/ext/matrix_float4x4.hpp"

#include "imgui.h"

#include <optional>

namespace sfs
{
class WebGpuRenderBackend;
}

// Shows the edited scene: the backend renders the scene into an offscreen
// target sized to this panel, which is drawn here as an image. Over it:
//   - click to select (ray picking against entity bounds),
//   - a move / rotate / scale gizmo on the selection (W / E / R, world or
//     local, hold Ctrl to snap),
//   - the selection's bounds outline and icons for lights,
//   - camera navigation (right-drag orbit, middle-drag pan, wheel zoom, F to
//     frame the selection).
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
  enum class Tool
  {
    Move,
    Rotate,
    Scale,
  };

  // The frame's camera matrices and where the image sits on screen.
  struct View
  {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::mat4 viewProjection{1.0f};
    ImVec2 origin{0.0f, 0.0f};
    ImVec2 size{0.0f, 0.0f};
  };

  View currentView(const EditorContext& context, ImVec2 origin, ImVec2 size) const;
  std::optional<ImVec2> toScreen(const View& view, const glm::vec3& world) const;

  void drawToolbar();
  void handleShortcuts(EditorContext& context);
  // True while the gizmo is hovered or dragging (so clicks don't pick).
  bool drawGizmo(EditorContext& context, const View& view);
  void pick(EditorContext& context, const View& view);
  void navigate(EditorContext& context, float viewportHeight);
  void drawSelectionOutline(const EditorContext& context, const View& view) const;
  void drawLightIcons(const EditorContext& context, const View& view) const;

  sfs::WebGpuRenderBackend& m_backend;
  sfs::WebGpuRenderTarget m_target;
  ViewportCamera m_navigation;

  Tool m_tool = Tool::Move;
  bool m_localSpace = false;
  bool m_snap = false;

  // The panel's image size from the last draw; the target takes it on the next
  // update, before the scene renders into it.
  glm::ivec2 m_desiredSize{0, 0};
};
