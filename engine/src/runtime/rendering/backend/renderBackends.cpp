#include "engine/runtime/rendering/backend/renderBackends.h"

#include <algorithm>
#include <utility>

namespace sfs
{

namespace
{
struct Offered
{
  std::string name;
  RenderBackends::Factory make;
};

std::vector<Offered>& offered()
{
  static std::vector<Offered> backends;
  return backends;
}
} // namespace

void RenderBackends::add(std::string name, Factory make)
{
  std::vector<Offered>& backends = offered();
  const auto it = std::find_if(backends.begin(),
                               backends.end(),
                               [&](const Offered& backend)
                               { return backend.name == name; });
  if (it != backends.end())
    it->make = std::move(make);
  else
    backends.push_back({std::move(name), std::move(make)});
}

std::unique_ptr<IRenderBackend> RenderBackends::make(std::string_view name)
{
  for (const Offered& backend : offered())
    if (backend.name == name)
      return backend.make();
  return nullptr;
}

std::vector<std::string> RenderBackends::names()
{
  std::vector<std::string> names;
  for (const Offered& backend : offered())
    names.push_back(backend.name);
  return names;
}

} // namespace sfs
