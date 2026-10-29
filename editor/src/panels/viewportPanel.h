#pragma once

#include "camera/viewportCamera.h"
#include "panels/panel.h"

#include "engine/runtime/rendering3d/picking.h"
#include "engine/webgpu/webGpuRenderTarget.h"
#include "glm/glm/ext/matrix_float4x4.hpp"

#include "imgui.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace sfs
{
class WebGpuRenderBackend;
}

// Shows the edited scene: the backend renders the scene into an offscreen
// target sized to this panel, which is drawn here as an image. Over it:
//   - click to select (ray picking against entity bounds),
//   - a move / rotate / scale gizmo on the selection (W / E / R, world or
//     local, hold Ctrl to snap),
//   - the selection's bounds outline and colliders, icons for lights and
//     outlines of what each camera entity sees
//     (these overlays and the gizmo toggle with "Gizmos", off during Play by
//     default),
//   - camera navigation (right-drag orbit, middle-drag pan, wheel zoom, F to
//     frame the selection),
//   - a grass brush (B, with grass selected): drag over its mesh to paint
//     where grass grows, Shift to erase.
//
// The editor can show several scenes (the scene, prefabs being edited), each
// in its own viewport; one is active at a time: the context is its scene, and
// it renders. The others show their last frame, and ask to become active when
// focused (a click, or their tab).
class ViewportPanel : public Panel
{
public:
  explicit ViewportPanel(sfs::WebGpuRenderBackend& backend, std::string title = "Viewport");
  ~ViewportPanel() override;

  void update(EditorContext& context) override;

  /** Render the active scene into this viewport (the app switches the context). */
  void setActive(bool active);
  bool active() const { return m_active; }
  /** True once after the inactive viewport was focused. */
  bool takeActivationRequest() { return std::exchange(m_activationRequested, false); }

  /** Extra controls on the toolbar, after its own (e.g. a prefab's Apply). */
  void setToolbarExtras(std::function<void(EditorContext&)> extras) { m_toolbarExtras = std::move(extras); }
  /** Mark the tab as having unsaved changes. */
  void setUnsaved(bool unsaved) { m_unsaved = unsaved; }
  /** On its first appearance, tab into the dock node of the window called
   * `window`. */
  void dockWith(std::string window) { m_dockWith = std::move(window); }

protected:
  void drawContents(EditorContext& context) override;
  int windowFlags() const override;
  void beforeBegin() override;

private:
  enum class Tool
  {
    Move,
    Rotate,
    Scale,
    GrassBrush, // paints the selection's GrassComponent
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

  bool gizmosVisible(const EditorContext& context) const;
  void drawToolbar(const EditorContext& context);
  void handleShortcuts(EditorContext& context);
  // True while the gizmo is hovered or dragging (so clicks don't pick).
  bool drawGizmo(EditorContext& context, const View& view);
  // Several selected: the gizmo in their middle, each moving, turning and
  // scaling as much as it does (about its own origin). `operation` is an
  // ImGuizmo::OPERATION.
  bool drawGroupGizmo(EditorContext& context,
                      const View& view,
                      const std::vector<sfs::Entity>& group,
                      int operation,
                      const float* snap);
  // The selected entities the gizmo moves: those with a place, not inside
  // another selected one.
  static std::vector<sfs::Entity> movableSelection(const EditorContext& context);
  void pick(EditorContext& context, const View& view);
  // The grass brush: its ring on the selection's mesh under the cursor, and a
  // stroke while the left button's down. True while over the image (so
  // clicks paint rather than pick).
  bool paintGrass(EditorContext& context, const View& view, bool hovered);
  void endGrassStroke(EditorContext& context);
  // A texture or material dragged from the Assets panel onto an object: into
  // the material slot of the part under the cursor.
  void dropAsset(EditorContext& context, const View& view);
  void navigate(EditorContext& context, float viewportHeight);
  void drawSelectionOutline(const EditorContext& context, const View& view) const;
  // An entity's world bounds with everything below it (a marker box if
  // there's no mesh in its branch).
  std::optional<sfs::Aabb> branchBounds(const EditorContext& context, const sfs::Entity& entity) const;
  // An entity's bounds as a box outline.
  void drawOutline(const EditorContext& context,
                   const View& view,
                   const sfs::Entity& entity,
                   ImU32 color,
                   float thickness) const;
  // What a drag would land on: its object outlined, and named by the cursor.
  void drawDropTarget(const EditorContext& context, const View& view, const sfs::SurfaceHit& hit) const;
  void drawLightIcons(const EditorContext& context, const View& view) const;
  void drawCameraIcons(const EditorContext& context, const View& view) const;
  void drawColliders(const EditorContext& context, const View& view) const;

  sfs::WebGpuRenderBackend& m_backend;
  sfs::WebGpuRenderTarget m_target;
  ViewportCamera m_navigation;

  Tool m_tool = Tool::Move;
  bool m_localSpace = false;
  glm::mat4 m_groupHandles{1.0f}; // the gizmo's, with several selected
  bool m_snap = false;
  bool m_gizmosInEdit = true;
  bool m_gizmosInPlay = false;
  bool m_animate = true; // while editing (see RenderSystem3D::setAnimating)

  // The grass brush: world units across, how fast it paints, erasing.
  float m_brushSize = 1.5f;
  float m_brushStrength = 0.6f;
  bool m_brushErase = false;
  int m_brushChannel = 0;     // which of the grass's maps (GrassMaps::Channel)
  bool m_brushStroke = false; // painting (an undo step open)

  bool m_active = false;
  bool m_activationRequested = false;
  bool m_unsaved = false;
  std::function<void(EditorContext&)> m_toolbarExtras;
  // Where the toolbar was drawn last frame (with its backdrop): the image
  // under it doesn't take clicks, so using the toolbar doesn't also pick or
  // paint.
  ImVec2 m_toolbarMin{0.0f, 0.0f};
  ImVec2 m_toolbarMax{0.0f, 0.0f};
  std::string m_dockWith;

  // The panel's image size from the last draw; the target takes it on the next
  // update, before the scene renders into it.
  glm::ivec2 m_desiredSize{0, 0};
};
