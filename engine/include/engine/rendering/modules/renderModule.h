#pragma once

#include "engine/rendering/commands/commands.h" // AnyRenderCommand
#include "engine/ecs/moduleSettings.h"
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
 * A unit of rendering composed into a render system via RenderModuleHost.
 * Registration is the enable. The host constructs + owns modules, init()s them,
 * forwards update(), and calls emit() each frame.
 *
 * Templated on the render context the module consumes (the isometric heightfield
 * context today, a flat-2D context later), so a module is matched at compile
 * time to the render systems that can host it.
 */
template <typename TContext>
class IRenderModule
{
public:
  virtual ~IRenderModule() = default;

  /** Receive registration-time dependencies (registry, asset store). */
  virtual void init(const ModuleInit&) {}

  /** Advance any per-frame simulation the module owns. */
  virtual void update(double /*deltaTime*/) {}

  /** Append this module's render commands for the frame. */
  virtual void emit(const TContext& context,
                    std::vector<AnyRenderCommand>& out) = 0;

  /**
   * UI-agnostic descriptors for this module's settings, for a debug panel or
   * editor. The render context lets a module expose only the settings that
   * apply to the active render mode (e.g. hide projected-shadow settings while
   * block geometry self-shadows). Default: none.
   */
  virtual std::vector<ModuleSetting> settings(const TContext&) { return {}; }
};

/** Helper base for modules that build a vector of one command type. */
template <typename TContext, typename TCommand>
class CommandModule : public IRenderModule<TContext>
{
public:
  /** Build the frame's commands into m_commands. */
  virtual void computeCommands(const TContext& context) = 0;

  const std::vector<TCommand>& commands() const { return m_commands; }

  void emit(const TContext& context,
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
