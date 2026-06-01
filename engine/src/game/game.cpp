#include "engine/rendering/openGLQuadRenderer.h"
#ifdef __EMSCRIPTEN__
  #include <GLES3/gl3.h>
#else
  #include <GL/glew.h>
#endif
#include <SDL_error.h>
#include <SDL_ttf.h>
#include <SDL_video.h>
#include <engine/game/game.h>

#include "SDL.h"
#include "engine/TextRenderer/textRenderer.h"

#include <engine/assetStore/sprite.h>
#include <engine/components/rigidBodyComponent.h>
#include <engine/components/spriteComponent.h>
#include <engine/components/transformComponent.h>
#include <engine/game/game.h>
#include <engine/logger/logger.h>
#include <engine/mapLoader/mapLoader.h>
#include <engine/systems/movementSystem.h>
#include <engine/systems/renderSystem.h>
#include <engine/utils/allocationMetrics.h>
#include <engine/utils/gpuProfiling.h>
#include <engine/utils/profiling.h>
#include <engine/utils/ui.h>
#include <glm/glm/ext/vector_float2.hpp>

#include <glm/glm/ext/vector_float3.hpp>
#include <memory>
#include <string>

#ifndef ENGINE_WEB
  #include "imgui.h"
  #include "imgui/backends/imgui_impl_sdlrenderer2.h" // IWYU pragma: keep
#endif

#ifdef __EMSCRIPTEN__
  #include <emscripten.h>
#endif

namespace sfs
{

bool Game::init(int windowWidth, int windowHeight)
{
  this->windowWidth = windowWidth;
  this->windowHeight = windowHeight;

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0)
  {
    LOG_ERROR(std::string("Error initializing SDL: ") + SDL_GetError());
    return false;
  }

  if (TTF_Init() != 0)
  {
    LOG_ERROR(std::string("Error initializing TTF: ") + TTF_GetError());
  }

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

  window = SDL_CreateWindow("SegFaultSimulator",
                            SDL_WINDOWPOS_CENTERED,
                            SDL_WINDOWPOS_CENTERED,
                            windowWidth,
                            windowHeight,
                            SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);

  if (!window)
  {
    LOG_ERROR("Error creating window");
    return false;
  }

  m_glContext = SDL_GL_CreateContext(window);

  if (!m_glContext)
  {
    LOG_ERROR("Failed to create OpenGL context");
    return false;
  }

  SDL_GL_MakeCurrent(window, m_glContext);

  // The iso pipeline relies on a real depth buffer for occlusion. If the driver
  // gave us 0 depth bits, depth-testing silently no-ops and occlusion breaks.
  int depthBits = 0;
  SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &depthBits);
  if (depthBits < 16)
    LOG_ERROR("GL context has no usable depth buffer (" +
              std::to_string(depthBits) + " bits); occlusion will be broken");

#ifndef __EMSCRIPTEN__
  SDL_GL_SetSwapInterval(0);

  glewExperimental = GL_TRUE;

  if (glewInit() != GLEW_OK)
  {
    LOG_ERROR("Failed to initialize GLEW");
    return false;
  }
#endif

  m_quadRenderer =
      std::make_unique<OpenGLQuadRenderer>(windowWidth, windowHeight);
  m_quadRenderer->initialize();
  m_quadRenderer->setViewportSize(windowWidth, windowHeight);

  // GL context is current and loaded; set up Tracy's GPU timing context.
  TracyGpuContext;

  // renderer = SDL_CreateRenderer(
  //     window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

  // if (!renderer)
  // {
  //   LOG_INFO(std::string("Error creating renderer: ") + SDL_GetError());
  //   return false;
  // }

  // SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN);

#ifndef ENGINE_WEB
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();

  ImGui_ImplSDL2_InitForOpenGL(window, m_glContext);
  ImGui_ImplOpenGL3_Init("#version 330");
#endif

  onInit();

  isRunning = true;

  return true;
}

void Game::setup()
{
  sfs::ScopedMemoryTracking tracking{sfs::MemoryTrackingPhase::Setup};

  assetStore = std::make_unique<AssetStore>();
  assetStore->addWhitePixelTexture("white_pixel");

  m_textRenderer = std::make_unique<TextRenderer>(*m_quadRenderer, *assetStore);
  m_textRenderer->init();

  sceneManager.setAssetStore(assetStore.get());
  sceneManager.setQuadRenderer(m_quadRenderer.get());
  sceneManager.setTextRenderer(m_textRenderer.get());

  onSetup();
}

void Game::run()
{
  previousTime = SDL_GetPerformanceCounter();

#ifdef __EMSCRIPTEN__

  // Store this pointer for the static loop
  static Game* self = this;

  auto loop = []()
  {
    if (!self->isRunning)
    {
      self->destroy();
      emscripten_cancel_main_loop(); // stop browser loop
      return;
    }

    Uint64 now = SDL_GetPerformanceCounter();
    double deltaTime = static_cast<double>(now - self->previousTime) /
                       SDL_GetPerformanceFrequency();

    self->previousTime = now;

    self->processInput();
    self->update(deltaTime);
    self->render();
  };

  emscripten_set_main_loop(loop, 0, 1);

#else

  while (isRunning)
  {
    Uint64 now = SDL_GetPerformanceCounter();
    double deltaTime =
        static_cast<double>(now - previousTime) / SDL_GetPerformanceFrequency();

    previousTime = now;

    processInput();
    update(deltaTime);
    render();
  }

  destroy();

#endif
}

void Game::processInput()
{
  ZoneScopedN("Game::processInput");

  input.mouse().beginFrame();

  SDL_Event sdlEvent;
  while (SDL_PollEvent(&sdlEvent))
  {

#ifndef ENGINE_WEB
    ImGui_ImplSDL2_ProcessEvent(&sdlEvent);
#endif

    if (sdlEvent.type == SDL_QUIT)
    {
      isRunning = false;
    }

    if (sdlEvent.type == SDL_MOUSEMOTION)
    {
      if (sdlEvent.motion.windowID != SDL_GetWindowID(window))
        continue;

      if (sdlEvent.motion.x < 0 || sdlEvent.motion.y < 0 ||
          sdlEvent.motion.x >= windowWidth || sdlEvent.motion.y >= windowHeight)
        continue;

      input.mouse().setPosition(sdlEvent.motion.x, sdlEvent.motion.y);
      // input.mouse().processDrag();
    }

    if (sdlEvent.type == SDL_MOUSEBUTTONDOWN)
    {
      input.mouse().setPosition(sdlEvent.button.x, sdlEvent.button.y);
      input.mouse().pressButton(
          static_cast<sfs::MouseButton>(sdlEvent.button.button));
    }

    if (sdlEvent.type == SDL_MOUSEBUTTONUP)
    {
      input.mouse().setPosition(sdlEvent.button.x, sdlEvent.button.y);
      input.mouse().releaseButton(
          static_cast<sfs::MouseButton>(sdlEvent.button.button));
    }

    if (sdlEvent.type == SDL_MOUSEWHEEL)
    {
      input.mouse().addScroll(sdlEvent.wheel.x, sdlEvent.wheel.y);
    }
  }

  // input.update contains SDL stuff which we don't want to track
  input.update();
  {
    sfs::ScopedMemoryTracking tracking{sfs::MemoryTrackingPhase::Input};

    sceneManager.current()->processInput(input);
    onProcessInput(input);
  }
}

void Game::update(double deltaTime)
{
  ZoneScopedN("Game::update");

  sfs::ScopedMemoryTracking tracking{sfs::MemoryTrackingPhase::Update};
  if (!sceneManager)
    return;
  sceneManager.current()->update(deltaTime);
  onUpdate(deltaTime);
}

void Game::render()
{
  ZoneScopedN("Game::render");

  // Render background
  glViewport(0, 0, windowWidth, windowHeight);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  // Depth write must be enabled for glClear to reset the depth buffer; the
  // translucent passes leave it disabled.
  glDepthMask(GL_TRUE);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  {
    if (!sceneManager)
      return;

    sceneManager.current()->render();

    sfs::ScopedMemoryTracking tracking{sfs::MemoryTrackingPhase::Render};
    onRender();
  }

#if !defined(NDEBUG) && !defined(ENGINE_WEB)
  renderDebugUI(sceneManager.current());
#endif

  SDL_GL_SwapWindow(window);

  // Collect this frame's GPU timing queries.
  TracyGpuCollect;
}

void Game::destroy()
{
  onDestroy();

  // Tear down GPU-backed resources while the GL context is still valid.
  m_textRenderer.reset();

  if (m_quadRenderer)
    m_quadRenderer->shutdown();
  m_quadRenderer.reset();

#ifndef ENGINE_WEB
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
#endif

  if (m_glContext)
  {
    SDL_GL_DeleteContext(m_glContext);
    m_glContext = nullptr;
  }

  if (renderer)
  {
    SDL_DestroyRenderer(renderer);
    renderer = nullptr;
  }

  if (window)
  {
    SDL_DestroyWindow(window);
    window = nullptr;
  }

  SDL_Quit();
}

} // namespace sfs
