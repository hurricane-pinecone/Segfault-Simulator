#include "engine/core/assets/scriptableObject.h"

#include <atomic>

namespace sfs
{

namespace
{
std::atomic<ScriptableObjectResolver> g_resolver{nullptr};
} // namespace

const ScriptableObject* findScriptableObject(std::string_view asset)
{
  const ScriptableObjectResolver resolver = g_resolver.load();
  return resolver ? resolver(asset) : nullptr;
}

void setScriptableObjectResolver(ScriptableObjectResolver resolver)
{
  g_resolver.store(resolver);
}

} // namespace sfs
