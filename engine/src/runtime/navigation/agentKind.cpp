#include "engine/runtime/navigation/agentKind.h"

#include <mutex>
#include <utility>

namespace sfs::nav
{

namespace
{
struct Kinds
{
  std::mutex mutex;
  std::vector<std::pair<std::string, std::shared_ptr<const AgentKind>>> list;
};
Kinds& kinds()
{
  static Kinds k;
  return k;
}
} // namespace

void AgentKinds::add(std::string name, std::shared_ptr<const AgentKind> kind)
{
  Kinds& k = kinds();
  std::lock_guard lock(k.mutex);
  for (auto& [n, f] : k.list)
    if (n == name)
    {
      f = std::move(kind);
      return;
    }
  k.list.emplace_back(std::move(name), std::move(kind));
}

std::shared_ptr<const AgentKind> AgentKinds::find(std::string_view name)
{
  Kinds& k = kinds();
  std::lock_guard lock(k.mutex);
  for (const auto& [n, f] : k.list)
    if (n == name)
      return f;
  return nullptr;
}

std::vector<std::string> AgentKinds::names()
{
  Kinds& k = kinds();
  std::lock_guard lock(k.mutex);
  std::vector<std::string> out;
  for (const auto& [n, f] : k.list)
    out.push_back(n);
  return out;
}

} // namespace sfs::nav
