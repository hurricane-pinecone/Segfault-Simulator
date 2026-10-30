#pragma once

#include "panels/panel.h"

#include "engine/runtime/rendering/backend/iRenderBackend.h"
#include "engine/runtime/rendering/iRenderer.h"
#include "engine/runtime/scripting/moduleHost.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sfs
{
class GameObject;
class Scene;
class UiModule;
struct SceneServices;
namespace ui
{
class Layer;
class Preview;
class PreviewStyle;
class View;
} // namespace ui
} // namespace sfs

class CodePanel;

// Game UI drawn as you write it, like SwiftUI's previews: the views in the
// file open in the Code panel, each as big as it is, by the real renderer.
// Each view struct in a .ui.h shows filled with placeholders; ui::Preview
// declarations show views with sample data. They come from the UI Preview's
// own module (the UI files, built in seconds as one's saved, apart from the
// game's), or the game's until that's built. A UI layer's file shows the
// layer on a screen: while playing, the game's own, live. Hovering a view
// outlines it, named with its size in points.
class UiPreviewPanel : public Panel
{
public:
  UiPreviewPanel(sfs::IRenderBackend& backend,
                 const sfs::SceneServices& services,
                 const CodePanel& code);
  ~UiPreviewPanel() override;

  /**
   * Let go of everything the game's module made (before it's unloaded or the
   * scenes are remade): the preview's copy of a layer.
   */
  void dropObjects();

  void update(EditorContext& context) override;

  /**
   * Show the previews of the UI Preview module at `module`, freshly built, in
   * place of the last one's.
   */
  void loadPreviewModule(const std::filesystem::path& module);
  /** Whether the UI files last failed to build (the panel says so). */
  void setBuildFailed(bool failed) { m_buildFailed = failed; }
  /**
   * Unload the UI Preview module (another project's opening): the game's
   * previews show until the next.
   */
  void dropPreviewModule();

protected:
  void drawContents(EditorContext& context) override;
  void beforeBegin() override;

private:
  // Every preview there is: the UI Preview module's (the UI files', fresh),
  // and the game's for the files it hasn't (scripts: a layer's).
  std::vector<const sfs::ui::Preview*> allPreviews() const;
  // The previews of the file shown (m_unit): its views', then its declared
  // ones.
  std::vector<const sfs::ui::Preview*> shownPreviews() const;
  // `view` in the styles declared in the file shown.
  sfs::ui::View styled(sfs::ui::View view) const;
  // The layer to show: the playing scene's, or the preview's copy (made now
  // if need be); null if there's none.
  sfs::ui::Layer* layer(EditorContext& context);
  // The UI module laying the UI out (made if need be), in the game's look.
  sfs::UiModule& uiModule(EditorContext& context);
  void followCode();
  void drawToolbar();
  // Lay `shown` out at `pixelsPerPoint` on a screen `screen` pixels big (or
  // as big as it is, if zero), with `room` pixels round it, and draw it and
  // the outlines.
  void drawUi(EditorContext& context,
              sfs::ui::Layer& shown,
              glm::vec2 screen,
              float pixelsPerPoint,
              float room,
              bool screenGuides);

  sfs::IRenderBackend& m_backend;
  const sfs::SceneServices& m_services;
  const CodePanel& m_code;
  std::unique_ptr<sfs::IRenderer> m_renderer;
  std::unique_ptr<sfs::IRenderTarget> m_target;
  bool m_initialized = false;
  bool m_failed = false;

  // A scene of the preview's own, holding its copy of a layer (so it has a
  // scene, as a script expects), and laying the UI out.
  std::unique_ptr<sfs::Scene> m_scene;
  std::unique_ptr<sfs::UiModule> m_ui;
  sfs::GameObject* m_copy = nullptr;
  std::string m_copyOf; // the script m_copy is

  // The UI Preview module, and the previews it made as it loaded.
  sfs::ModuleHost m_previewModule;
  std::vector<const sfs::ui::Preview*> m_modulePreviews;
  std::vector<const sfs::ui::PreviewStyle*> m_moduleStyles;
  std::size_t m_previewLoads = 0; // how many times it's been loaded
  bool m_buildFailed = false;

  std::string m_unit; // the file shown ("objects/FarmHud": its .h, .cpp...)
  std::string m_layerName; // its UI layer, if it's one
  bool m_follow = true;    // show the file the Code panel has open
  std::filesystem::path m_followed;
  std::size_t m_codeSeen = 0; // the module's scripts and previews when followed
  bool m_bringForward = false; // show this tab (a UI file was opened)
  bool m_dockChecked = false;
  bool m_outlines = true;
  std::string m_error; // a view threw
};
