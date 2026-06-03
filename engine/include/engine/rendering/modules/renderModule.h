#pragma once

#include "engine/rendering/commands/commands.h" // AnyRenderCommand
#include "engine/rendering/isometricRenderContext.h"
#include <vector>

namespace sfs
{

class Registry;
class AssetStore;

/** Dependencies injected into a render module at registration. */
struct ModuleInit
{
  Registry* registry = nullptr;
  AssetStore* assetStore = nullptr;
};

/**
 * A unit of rendering composed into IsometricRenderSystem via withModule<T>().
 * Registration is the enable. The render system constructs + owns modules,
 * init()s them, forwards update(), and calls emit() each frame.
 */
class IRenderModule
{
public:
  virtual ~IRenderModule() = default;

  /** Receive registration-time dependencies (registry, asset store). */
  virtual void init(const ModuleInit&) {}

  /** Advance any per-frame simulation the module owns. */
  virtual void update(double /*deltaTime*/) {}

  /** Append this module's render commands for the frame. */
  virtual void emit(const IsometricRenderContext& context,
                    std::vector<AnyRenderCommand>& out) = 0;
};

/** Helper base for modules that build a vector of one command type. */
template <typename TCommand>
class CommandModule : public IRenderModule
{
public:
  /** Build the frame's commands into m_commands. */
  virtual void computeCommands(const IsometricRenderContext& context) = 0;

  const std::vector<TCommand>& commands() const { return m_commands; }

  void emit(const IsometricRenderContext& context,
            std::vector<AnyRenderCommand>& out) override
  {
    computeCommands(context);
    // TCommand -> AnyRenderCommand.
    out.insert(out.end(), m_commands.begin(), m_commands.end());
  }

protected:
  void flush() { m_commands.clear(); }

  std::vector<TCommand> m_commands;
};

} // namespace sfs
