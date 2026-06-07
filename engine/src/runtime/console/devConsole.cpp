#include "engine/runtime/console/devConsole.h"

#include "engine/core/Color/Color.h"
#include "engine/core/rendering/quads.h"
#include "engine/core/scripting/luaScripting.h"
#include "engine/runtime/TextRenderer/textRenderer.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/rendering/iQuadRenderer.h"

#include <SDL.h>

#include <algorithm>

namespace sfs
{

namespace
{
// Break evaluator output into screen lines. Each '\n' starts a new line and
// tabs become spaces, since the single-line text renderer draws neither.
std::vector<std::string> toLines(const std::string& output)
{
  std::vector<std::string> lines;
  std::string current;
  for (const char c : output)
  {
    if (c == '\n')
    {
      lines.push_back(current);
      current.clear();
    }
    else if (c == '\t')
    {
      current += "  ";
    }
    else
    {
      current.push_back(c);
    }
  }
  if (!current.empty())
    lines.push_back(current);
  return lines;
}
} // namespace

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
    m_cursor = 0;
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
        m_input.insert(m_cursor++, 1, *c);
    m_historyPos = m_history.size(); // editing leaves history recall
    return true;
  }

  if (event.type == SDL_KEYDOWN)
  {
    switch (event.key.keysym.sym)
    {
    case SDLK_BACKSPACE:
      if (m_cursor > 0)
        m_input.erase(--m_cursor, 1);
      m_historyPos = m_history.size();
      break;
    case SDLK_DELETE:
      if (m_cursor < m_input.size())
        m_input.erase(m_cursor, 1);
      m_historyPos = m_history.size();
      break;
    case SDLK_LEFT:
      if (m_cursor > 0)
        --m_cursor;
      break;
    case SDLK_RIGHT:
      if (m_cursor < m_input.size())
        ++m_cursor;
      break;
    case SDLK_HOME:
      m_cursor = 0;
      break;
    case SDLK_END:
      m_cursor = m_input.size();
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
  m_cursor = 0;
}

void DevConsole::recallOlder()
{
  if (m_historyPos == 0)
    return; // already at the oldest entry

  --m_historyPos;
  m_input = m_history[m_historyPos];
  m_cursor = m_input.size();
}

void DevConsole::recallNewer()
{
  if (m_historyPos >= m_history.size())
    return; // already on the live line

  ++m_historyPos;
  m_input =
      m_historyPos < m_history.size() ? m_history[m_historyPos] : std::string{};
  m_cursor = m_input.size();
}

void DevConsole::render(TextRenderer& text,
                        IQuadRenderer& quads,
                        AssetStore& assets,
                        int viewportWidth,
                        int viewportHeight) const
{
  if (!m_open)
    return;

  constexpr const char* font = "console";
  constexpr int pad = 4;
  constexpr int marginX = 8;

  int lineH = text.lineHeight(font);
  if (lineH <= 0)
    lineH = 16;

  // The result (or error) sits above the input, one screen line per '\n'. Cap
  // the block to most of the frame so a large table can't cover everything;
  // the overflow is dropped with a marker.
  std::vector<std::string> lines = toLines(m_output);
  const int maxLines = std::max(1, (viewportHeight * 3 / 5) / lineH);
  if (static_cast<int>(lines.size()) > maxLines)
  {
    lines.resize(maxLines);
    lines.back() = "...";
  }

  const int outputHeight = static_cast<int>(lines.size()) * lineH;
  const int barHeight = pad + outputHeight + lineH + pad; // + the input line
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

  // Output lines, top to bottom, then the single input line at the bottom.
  int y = barTop + pad;
  for (const std::string& outputLine : lines)
  {
    if (!outputLine.empty())
      text.drawText(static_cast<float>(marginX),
                    static_cast<float>(y),
                    outputLine,
                    font,
                    Colors::LightGray);
    y += lineH;
  }

  // Blinking caret at the cursor (~2 Hz). It occupies one monospace cell either
  // way (bar or space), so the text past it does not jump as it blinks.
  const bool blink = (SDL_GetTicks() / 500) % 2 == 0;
  const std::string line = "> " + m_input.substr(0, m_cursor) +
                           (blink ? "|" : " ") + m_input.substr(m_cursor);
  text.drawText(
      static_cast<float>(marginX), static_cast<float>(y), line, font, Colors::White);
}

} // namespace sfs
