
#pragma once

#include <vector>

namespace sfs
{

template <typename TContext, typename TCommand>
class RenderProvider
{
public:
  virtual ~RenderProvider() = default;

  virtual void computeCommands(const TContext& context) = 0;
  const std::vector<TCommand>& commands() const { return m_commands; };

protected:
  void flush() { m_commands.clear(); }

protected:
  std::vector<TCommand> m_commands;
};

} // namespace sfs
