#pragma once

#include <stdexcept>
#include <string>

namespace sfs
{

/**
 * A broken precondition in game code that the engine can't carry on past:
 * reading a component from a dead entity, asking for a system the scene
 * doesn't have, too many component types. Thrown by fatalError(); the game
 * loop catches it, tells the player (a message box naming the log) and ends
 * the game cleanly. Through a Lua binding it arrives as a Lua error.
 */
class FatalError : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

/** Log `message` as an error and throw it as a FatalError. */
[[noreturn]] void fatalError(const std::string& message);

} // namespace sfs
