#include "engine/runtime/console/devConsole.h"

#include "engine/core/Color/Color.h"
#include "engine/core/rendering/quads.h"
#include "engine/core/scripting/luaScripting.h"
#include "engine/runtime/TextRenderer/textRenderer.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/rendering/iQuadRenderer.h"

#include <SDL.h>

namespace sfs
{

void DevConsole::toggle()
{
  m_open = !m_open;
  if (m_open)
  {
    SDL_StartTextInput();
  }
  else
  {
    SDL_StopTextInput();
    m_input.clear();
  }
}

bool DevConsole::handleEvent(const SDL_Event& event)
{
  // The backtick is the toggle key from any state, never a typed character.
  if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_BACKQUOTE)
  {
    toggle();
    return true;
  }

  if (!m_open)
    return false;

  if (event.type == SDL_TEXTINPUT)
  {
    for (const char* c = event.text.text; *c != '\0'; ++c)
      if (*c != '`') // the toggle key never lands in the buffer
        m_input.push_back(*c);
    m_historyPos = m_history.size(); // editing leaves history recall
    return true;
  }

  if (event.type == SDL_KEYDOWN)
  {
    switch (event.key.keysym.sym)
    {
    case SDLK_BACKSPACE:
      if (!m_input.empty())
        m_input.pop_back();
      m_historyPos = m_history.size();
      break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      submit();
      break;
    case SDLK_UP:
      recallOlder();
      break;
    case SDLK_DOWN:
      recallNewer();
      break;
    case SDLK_ESCAPE:
      toggle(); // close
      break;
    default:
      break;
    }
    return true; // swallow every key while the console has focus
  }

  return false;
}

void DevConsole::submit()
{
  if (m_input.empty())
    return;

  // Keep it in history (skip a straight repeat of the last command).
  if (m_history.empty() || m_history.back() != m_input)
    m_history.push_back(m_input);
  m_historyPos = m_history.size();

  LuaScripting* lua = activeLua();
  m_output = lua ? lua->evalRepl(m_input) : "error: no Lua VM";

  m_input.clear();
}

void DevConsole::recallOlder()
{
  if (m_historyPos == 0)
    return; // already at the oldest entry

  --m_historyPos;
  m_input = m_history[m_historyPos];
}

void DevConsole::recallNewer()
{
  if (m_historyPos >= m_history.size())
    return; // already on the live line

  ++m_historyPos;
  m_input =
      m_historyPos < m_history.size() ? m_history[m_historyPos] : std::string{};
}

void DevConsole::render(TextRenderer& text,
                        IQuadRenderer& quads,
                        AssetStore& assets,
                        int viewportWidth,
                        int viewportHeight) const
{
  if (!m_open)
    return;

  constexpr int barHeight = 28;
  const int barTop = viewportHeight - barHeight;

  // Dark translucent bar from the 1x1 white_pixel texture (no dedicated
  // shader).
  if (SDL_Surface* pixel = assets.getSurface("white_pixel"))
  {
    TexturedQuad bar;
    bar.texture = quads.getOrCreateTexture("white_pixel", pixel);
    bar.srcRect = {0, 0, 1, 1};
    bar.destRect = {0, barTop, viewportWidth, barHeight};
    bar.textureWidth = 1;
    bar.textureHeight = 1;
    bar.tint = {12, 12, 16, 220};
    quads.drawImmediate(bar);
  }

  // Last result (or error) sits just above the input line.
  if (!m_output.empty())
    text.drawText(8.0f,
                  static_cast<float>(barTop - barHeight),
                  m_output,
                  Colors::LightGray);

  // Blinking block cursor at the end of the line (~2 Hz).
  const bool blink = (SDL_GetTicks() / 500) % 2 == 0;
  const std::string line = "> " + m_input + (blink ? "_" : "");
  text.drawText(8.0f, static_cast<float>(barTop + 2), line, Colors::White);
}

} // namespace sfs
