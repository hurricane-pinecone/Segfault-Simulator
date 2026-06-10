#pragma once

#include "engine/runtime/rendering/backend/iRenderBackend.h"
#include "engine/runtime/sceneManager/scene.h"

#include <SDL_video.h>

#include <functional>
#include <memory>
#include <optional>

namespace sfs
{

class AssetStore;
class IQuadRenderer;
class TextRenderer;

// OpenGL render backend: an SDL GL window + context, the quad renderer the game
// draws through, and the asset/text services built on it. The quad-renderer
// flavour (2D vs isometric) is chosen by the factory the Game passes in.
class GLRenderBackend : public IRenderBackend
{
public:
  using QuadRendererFactory =
      std::function<std::unique_ptr<IQuadRenderer>(int, int)>;

  explicit GLRenderBackend(QuadRendererFactory factory);
  ~GLRenderBackend() override;

  bool init(const char* title, int width, int height) override;
  SDL_Window* window() const override { return m_window; }
  void onResize(int width, int height) override;
  SceneServices* sceneServices() override;

  void beginFrame(int width, int height) override;
  void endFrame() override;

  void imguiProcessEvent(const SDL_Event& event) override;
  void imguiNewFrame() override;
  void imguiRenderDrawData() override;
  bool imguiAvailable() const override;

  void shutdown() override;

private:
  QuadRendererFactory m_makeQuadRenderer;

  SDL_Window* m_window = nullptr;
  SDL_GLContext m_glContext = nullptr;

  std::unique_ptr<IQuadRenderer> m_quadRenderer;
  std::unique_ptr<AssetStore> m_assetStore;
  std::unique_ptr<TextRenderer> m_textRenderer;

  // Bundle of references into the three services above; handed to the
  // SceneManager. Built once the services exist.
  std::optional<SceneServices> m_services;
};

} // namespace sfs
