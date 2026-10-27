#include "panels/viewportPanel.h"

#include "editorContext.h"

#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/core/components/transform3DComponent.h"
#include "engine/runtime/rendering3d/renderSystem3D.h"
#include "engine/webgpu/webGpuRenderBackend.h"

#include "imgui.h"

ViewportPanel::ViewportPanel(sfs::WebGpuRenderBackend& backend)
    : Panel("Viewport"), m_backend(backend), m_target(backend.context())
{
  m_backend.setSceneTarget(&m_target);
}

ViewportPanel::~ViewportPanel() { m_backend.setSceneTarget(nullptr); }

int ViewportPanel::windowFlags() const
{
  // The image fills the panel; scrolling would only fight camera zoom.
  return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
}

void ViewportPanel::update(EditorContext&)
{
  // Resize before the scene renders: the frame's ImGui draw (later this frame)
  // samples the target, so it must not be replaced after the panel is drawn.
  m_target.resize(open() ? m_desiredSize : glm::ivec2(0, 0));
}

void ViewportPanel::drawContents(EditorContext& context)
{
  const ImVec2 available = ImGui::GetContentRegionAvail();
  m_desiredSize = {static_cast<int>(available.x), static_cast<int>(available.y)};

  if (m_target.valid() && m_target.size() == m_desiredSize)
  {
    ImGui::Image(static_cast<ImTextureID>(
                     reinterpret_cast<intptr_t>(m_target.view())),
                 available);
    if (ImGui::IsItemHovered())
      navigate(context, available.y);
  }
  else
  {
    // First frame, or mid-resize: the correctly sized target arrives next
    // frame.
    ImGui::Dummy(available);
  }
}

void ViewportPanel::navigate(EditorContext& context, float viewportHeight)
{
  if (!context.render)
    return;

  const ImGuiIO& io = ImGui::GetIO();
  ViewportCamera::Input input;
  input.mouseDelta = {io.MouseDelta.x, io.MouseDelta.y};
  input.wheel = io.MouseWheel;
  input.pan = ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
              (io.KeyShift && ImGui::IsMouseDown(ImGuiMouseButton_Right));
  input.orbit = !input.pan && ImGui::IsMouseDown(ImGuiMouseButton_Right);

  sfs::Camera3D& camera = context.render->camera();
  m_navigation.apply(camera, input, viewportHeight);

  // F frames the selection.
  if (ImGui::IsKeyPressed(ImGuiKey_F) && context.hasSelection() &&
      context.selection.hasComponent<sfs::Transform3DComponent>())
    m_navigation.focus(
        camera,
        context.selection.getComponent<sfs::Transform3DComponent>().position);
}
