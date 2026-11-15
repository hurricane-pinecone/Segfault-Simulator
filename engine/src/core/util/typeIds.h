#pragma once

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

// engine-core's own: the tables behind componentTypeId(), systemTypeId() and
// eventTypeId(). Kept out of the public headers, which only need the ids.
namespace sfs::detail
{

/**
 * Ids by type key, numbered from 0 in the order they're first asked for. Each
 * kind of id (components', systems', events') is one table defined once in
 * engine-core, so every module that asks gets the same id. Thread-safe.
 */
class TypeIds
{
public:
  /**
   * The id of a type key, assigning the next free one on first use.
   * @param typeKey the type's key, componentTypeKey<T>()
   * @param added if given, set to whether the id was assigned by this call
   * @return its id, counting from 0
   */
  int idOf(std::string_view typeKey, bool* added = nullptr);

private:
  std::mutex m_mutex;
  std::unordered_map<std::string, int> m_ids;
};

} // namespace sfs::detail
