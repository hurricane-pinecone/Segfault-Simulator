#include "engine/core/sceneData/shaderGlobals.h"

#include "engine/core/logger/logger.h"

#include <atomic>
#include <mutex>

namespace sfs
{

namespace
{

// The process's parameter names, slot by slot. Names are only ever added
// (under the mutex, then published by `count`), so a reader scans the first
// `count` without a lock.
struct SlotTable
{
  std::array<std::string, kMaxShaderGlobals> names{"wetness", "puddles"};
  std::atomic<std::size_t> count{2};
  std::mutex adding;
  std::atomic<bool> warned{false};
};

SlotTable& slotTable()
{
  static SlotTable table;
  return table;
}

int findSlot(const SlotTable& table, std::string_view name, std::size_t count)
{
  for (std::size_t i = 0; i < count; ++i)
    if (table.names[i] == name)
      return static_cast<int>(i);
  return -1;
}

} // namespace

int findShaderGlobalSlot(std::string_view name)
{
  if (name.empty())
    return -1;
  const SlotTable& table = slotTable();
  return findSlot(table, name, table.count.load(std::memory_order_acquire));
}

int shaderGlobalSlot(std::string_view name)
{
  if (name.empty())
    return -1;
  SlotTable& table = slotTable();
  if (const int slot =
          findSlot(table, name, table.count.load(std::memory_order_acquire));
      slot >= 0)
    return slot;

  const std::lock_guard<std::mutex> lock(table.adding);
  const std::size_t count = table.count.load(std::memory_order_relaxed);
  if (const int slot = findSlot(table, name, count); slot >= 0)
    return slot;
  if (count >= kMaxShaderGlobals)
  {
    if (!table.warned.exchange(true))
      LOG_WARN("Global shader parameter '" + std::string(name) +
               "' has no slot: all " + std::to_string(kMaxShaderGlobals) +
               " are taken; it reads 0 (and so does any other past them)");
    return -1;
  }
  table.names[count] = std::string(name);
  table.count.store(count + 1, std::memory_order_release);
  return static_cast<int>(count);
}

const glm::vec4* shaderGlobal(const ShaderGlobals& globals,
                              std::string_view name)
{
  for (const ShaderParameter& parameter : globals.parameters)
    if (parameter.name == name)
      return &parameter.value;
  return nullptr;
}

void setShaderGlobal(ShaderGlobals& globals,
                     std::string_view name,
                     const glm::vec4& value)
{
  for (ShaderParameter& parameter : globals.parameters)
    if (parameter.name == name)
    {
      parameter.value = value;
      return;
    }
  globals.parameters.push_back(ShaderParameter{std::string(name), value});
}

void setShaderGlobal(ShaderGlobals& globals, std::string_view name, float value)
{
  setShaderGlobal(globals, name, glm::vec4(value, 0.0f, 0.0f, 0.0f));
}

void gatherShaderGlobals(const ShaderGlobals* globals,
                         ShaderGlobalValues& values)
{
  values.fill(glm::vec4(0.0f));
  if (!globals)
    return;
  for (const ShaderParameter& parameter : globals->parameters)
    if (const int slot = findShaderGlobalSlot(parameter.name); slot >= 0)
      values[static_cast<std::size_t>(slot)] = parameter.value;
}

} // namespace sfs
