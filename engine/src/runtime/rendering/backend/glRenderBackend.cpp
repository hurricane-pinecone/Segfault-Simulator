#include "engine/runtime/rendering/backend/glRenderBackend.h"

#include "engine/runtime/rendering/gl/openGLQuadRenderer.h"
#ifdef __EMSCRIPTEN__
  #include <GLES3/gl3.h>
#else
  #include <GL/glew.h>
#endif

#include "engine/core/logger/logger.h"
#include "engine/core/util/profiling.h"
#include "engine/runtime/TextRenderer/textRenderer.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/rendering/gl/gpuProfiling.h"

#include <SDL_video.h>

#include <string>
#include <utility>

#ifndef ENGINE_WEB
  #include "imgui.h"
  #include "imgui/backends/imgui_impl_opengl3.h"
  #include "imgui/backends/imgui_impl_sdl2.h"
#endif

namespace sfs
{

GLRenderBackend::GLRenderBackend(QuadRendererFactory factory)
    : m_makeQuadRenderer(std::move(factory))
{
}

GLRenderBackend::~GLRenderBackend() { shutdown(); }

bool GLRenderBackend::init(const char* title, int width, int height)
{
  SDL_DisplayMode displayMode;
  SDL_GetCurrentDisplayMode(0, &displayMode);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
#ifdef __EMSCRIPTEN__
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#else
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif

  m_window = SDL_CreateWindow(title,
                              SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED,
                              width,
                              height,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
  if (!m_window)
  {
    LOG_ERROR("Error creating window");
    return false;
  }

  m_glContext = SDL_GL_CreateContext(m_window);
  if (!m_glContext)
  {
    LOG_ERROR("Failed to create OpenGL context");
    return false;
  }

  SDL_GL_MakeCurrent(m_window, m_glContext);

  // The iso pipeline relies on a real depth buffer for occlusion; with too few
  // depth bits, depth-testing silently no-ops and occlusion breaks.
  int depthBits = 0;
  SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &depthBits);
  if (depthBits < 16)
  {
    LOG_ERROR("GL context has no usable depth buffer (" +
              std::to_string(depthBits) + " bits); occlusion would be broken");
    return false;
  }

#ifndef __EMSCRIPTEN__
  SDL_GL_SetSwapInterval(0);

  glewExperimental = GL_TRUE;

  if (glewInit() != GLEW_OK)
  {
    LOG_ERROR("Failed to initialize GLEW");
    return false;
  }
#endif

  m_quadRenderer = m_makeQuadRenderer(width, height);
  if (!m_quadRenderer->initialize())
  {
    LOG_ERROR("Failed to initialize quad renderer");
    return false;
  }
  m_quadRenderer->setViewportSize(width, height);

  // GL context is current and loaded; set up Tracy's GPU timing context.
  TracyGpuContext;

  m_assetStore = std::make_unique<AssetStore>();
  m_assetStore->addWhitePixelTexture("white_pixel");
  // Round sibling of white_pixel: a white dot with a radial alpha falloff, so
  // particles/decals read as soft circles instead of hard squares. Tinted by
  // colour at draw time, so it needs no art.
  m_assetStore->addRadialTexture("white_dot", 32);

  m_textRenderer =
      std::make_unique<TextRenderer>(*m_quadRenderer, *m_assetStore);
  m_textRenderer->init();

  m_services.emplace(
      SceneServices{*m_assetStore, *m_quadRenderer, *m_textRenderer});

#ifndef ENGINE_WEB
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui_ImplSDL2_InitForOpenGL(m_window, m_glContext);
  ImGui_ImplOpenGL3_Init("#version 330");
#endif

  return true;
}

SceneServices* GLRenderBackend::sceneServices()
{
  return m_services ? &*m_services : nullptr;
}

void GLRenderBackend::onResize(int width, int height)
{
  if (m_quadRenderer)
    m_quadRenderer->setViewportSize(width, height);
  glViewport(0, 0, width, height);
}

void GLRenderBackend::beginFrame(int width, int height)
{
  glViewport(0, 0, width, height);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  // Depth write must be enabled for glClear to reset the depth buffer; the
  // translucent passes leave it disabled.
  glDepthMask(GL_TRUE);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void GLRenderBackend::endFrame()
{
  {
    // CPU blocks here until the GPU has presented the frame; zoning it keeps
    // that wait out of the caller's unattributed self-time.
    ZoneScopedN("SDL_GL_SwapWindow");
    SDL_GL_SwapWindow(m_window);
  }

  // Collect this frame's GPU timing queries.
  TracyGpuCollect;
}

void GLRenderBackend::imguiProcessEvent(const SDL_Event& event)
{
#ifndef ENGINE_WEB
  ImGui_ImplSDL2_ProcessEvent(&event);
#endif
}

void GLRenderBackend::imguiNewFrame()
{
#ifndef ENGINE_WEB
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();
#endif
}

void GLRenderBackend::imguiRenderDrawData()
{
#ifndef ENGINE_WEB
  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#endif
}

bool GLRenderBackend::imguiAvailable() const
{
#ifndef ENGINE_WEB
  return true;
#else
  return false;
#endif
}

void GLRenderBackend::shutdown()
{
  // Tear down GPU-backed resources while the GL context is still valid.
  m_services.reset();
  m_textRenderer.reset();

  if (m_quadRenderer)
    m_quadRenderer->shutdown();
  m_quadRenderer.reset();
  m_assetStore.reset();

#ifndef ENGINE_WEB
  if (ImGui::GetCurrentContext())
  {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
  }
#endif

  if (m_glContext)
  {
    SDL_GL_DeleteContext(m_glContext);
    m_glContext = nullptr;
  }

  if (m_window)
  {
    SDL_DestroyWindow(m_window);
    m_window = nullptr;
  }
}

} // namespace sfs
