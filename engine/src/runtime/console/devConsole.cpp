#include "engine/runtime/console/devConsole.h"

#include "engine/core/scripting/luaScripting.h"

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
  m_layer.visible = m_open;
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

ui::View DevConsole::view() const
{
  using namespace ui;
  // The result (or error) above the input, a line a line, capped so a big
  // table can't cover the screen; then the prompt with a blinking caret (one
  // cell either way, so the text past it doesn't jump).
  std::vector<std::string> lines = toLines(m_output);
  constexpr std::size_t kMostLines = 24;
  if (lines.size() > kMostLines)
  {
    lines.resize(kMostLines);
    lines.back() = "...";
  }
  const bool blink = (SDL_GetTicks() / 500) % 2 == 0;
  std::vector<View> rows;
  for (const std::string& line : lines)
    rows.push_back(
        Text(line.empty() ? " " : line).color(Color::rgb(200, 200, 200)));
  rows.push_back(Text("> " + m_input.substr(0, m_cursor) + (blink ? "|" : " ") +
                      m_input.substr(m_cursor)));
  return HStack{VStack(std::move(rows), Alignment::Leading), Spacer()}
      .padding(8)
      .background(Color::rgb(12, 12, 16, 0.86f))
      .align(Alignment::BottomLeading);
}

} // namespace sfs
