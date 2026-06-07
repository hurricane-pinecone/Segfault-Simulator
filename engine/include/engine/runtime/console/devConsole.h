#pragma once

#include <SDL_events.h>
#include <cstddef>
#include <string>
#include <vector>

namespace sfs
{

class TextRenderer;
class IQuadRenderer;
class AssetStore;

/**
 * In-app developer console: a single-line input with multi-line output. Toggle
 * with the backtick key, type a Lua statement, and press Enter to run it against
 * the live VM (sfs::activeLua). A table result prints pretty, one key per line.
 *
 * It draws through the engine's TextRenderer, so it carries no ImGui dependency
 * and ships in every build -- on web it shares the VM the on-page editor drives.
 * Input is fed from the host's SDL event stream; while the console is open the
 * host withholds input from the game (keyboard state is polled, so swallowing
 * events alone wouldn't stop the player moving as you type).
 */
class DevConsole
{
public:
  bool isOpen() const { return m_open; }

  // Flip visibility, starting/stopping SDL text input to match.
  void toggle();

  // Feed one SDL event. Returns true when the console consumed it, so the host
  // skips its own handling. Backtick toggles from any state; while open, this
  // captures typing, Backspace/Delete, Left/Right/Home/End (move the caret),
  // Enter (runs the line), Up/Down (recall previous commands), and Escape
  // (closes).
  bool handleEvent(const SDL_Event& event);

  // Draw the console bar (background, last result, prompt + input) along the
  // bottom of a viewportWidth x viewportHeight frame. No-op while closed.
  void render(TextRenderer& text,
              IQuadRenderer& quads,
              AssetStore& assets,
              int viewportWidth,
              int viewportHeight) const;

private:
  // Run the current line against the live VM and keep its result/error to show.
  void submit();

  // Walk the submitted-command history into the input line (older / newer).
  void recallOlder();
  void recallNewer();

  bool m_open = false;
  std::string m_input;
  std::size_t m_cursor = 0; // caret position within m_input (0..size)
  std::string m_output; // last result or error, shown above the input line

  // Submitted commands, oldest first. m_historyPos is the recall cursor: it
  // equals m_history.size() when not browsing (the live, editable line).
  std::vector<std::string> m_history;
  std::size_t m_historyPos = 0;
};

} // namespace sfs
