#include "scripting/fieldMigration.h"

#include <algorithm>
#include <cstddef>
#include <optional>

namespace fieldMigration
{

namespace
{
using nlohmann::json;

struct Indexed
{
  std::size_t index;
  Field field;
};

// A type's changes between two layouts of it (empty if none).
TypeChanges compare(const Layout& before, const Layout& after)
{
  const auto has = [](const Layout& layout, const std::string& name)
  {
    for (const Field& field : layout.fields)
      if (field.name == name)
        return true;
    return false;
  };
  std::vector<Indexed> removed, added;
  for (std::size_t i = 0; i < before.fields.size(); ++i)
    if (!has(after, before.fields[i].name))
      removed.push_back({i, before.fields[i]});
  for (std::size_t i = 0; i < after.fields.size(); ++i)
    if (!has(before, after.fields[i].name))
      added.push_back({i, after.fields[i]});

  TypeChanges changes;
  std::vector<bool> addedUsed(added.size(), false), removedUsed(removed.size(), false);
  // Renamed in place: the same position and kind.
  for (std::size_t r = 0; r < removed.size(); ++r)
    for (std::size_t a = 0; a < added.size(); ++a)
      if (!addedUsed[a] && added[a].index == removed[r].index && added[a].field.kind == removed[r].field.kind)
      {
        changes.renamed[removed[r].field.name] = added[a].field.name;
        addedUsed[a] = removedUsed[r] = true;
        break;
      }
  // Otherwise, as many gone as appeared, kinds matching in order (fields
  // moved about as well as renamed).
  std::vector<std::size_t> restRemoved, restAdded;
  for (std::size_t r = 0; r < removed.size(); ++r)
    if (!removedUsed[r])
      restRemoved.push_back(r);
  for (std::size_t a = 0; a < added.size(); ++a)
    if (!addedUsed[a])
      restAdded.push_back(a);
  bool inOrder = !restRemoved.empty() && restRemoved.size() == restAdded.size();
  for (std::size_t i = 0; inOrder && i < restRemoved.size(); ++i)
    inOrder = removed[restRemoved[i]].field.kind == added[restAdded[i]].field.kind;
  if (inOrder)
    for (std::size_t i = 0; i < restRemoved.size(); ++i)
    {
      changes.renamed[removed[restRemoved[i]].field.name] = added[restAdded[i]].field.name;
      removedUsed[restRemoved[i]] = true;
    }
  for (std::size_t r = 0; r < removed.size(); ++r)
    if (!removedUsed[r])
      changes.removed.insert(removed[r].field.name);
  return changes;
}

// An object's fields renamed and dropped. @return whether it changed
bool rewrite(json& fields, const TypeChanges& changes)
{
  if (!fields.is_object())
    return false;
  json out = json::object();
  bool changed = false;
  // Kept fields first, so a renamed one never overwrites a value already there.
  for (auto& [name, value] : fields.items())
    if (!changes.renamed.contains(name) && !changes.removed.contains(name))
      out[name] = std::move(value);
  for (auto& [name, value] : fields.items())
  {
    if (changes.removed.contains(name))
      changed = true;
    else if (const auto renamed = changes.renamed.find(name); renamed != changes.renamed.end())
    {
      if (!out.contains(renamed->second))
        out[renamed->second] = std::move(value);
      changed = true;
    }
  }
  if (changed)
    fields = std::move(out);
  return changed;
}

// For a script's fields saved without the script's name (an override of an
// instance, which only names what changed): the changes that can't be meant
// for another script.
TypeChanges unambiguousScriptChanges(const Changes& changes)
{
  std::map<std::string, std::optional<std::string>> renames; // nullopt: conflicting
  std::set<std::string> removed;
  for (const auto& [script, type] : changes.scripts)
  {
    for (const auto& [from, to] : type.renamed)
    {
      const auto [it, fresh] = renames.try_emplace(from, to);
      if (!fresh && it->second != to)
        it->second.reset();
    }
    removed.insert(type.removed.begin(), type.removed.end());
  }
  TypeChanges out;
  for (const auto& [from, to] : renames)
    if (to && !changes.scriptFieldsNow.contains(from) && !removed.contains(from))
      out.renamed[from] = *to;
  for (const std::string& name : removed)
    if (!changes.scriptFieldsNow.contains(name) && !renames.contains(name))
      out.removed.insert(name);
  return out;
}

bool walk(json& data, const Changes& changes, const TypeChanges& unnamedScript)
{
  bool changed = false;
  if (data.is_object())
  {
    // Components under a renamed type's name: under its new one.
    std::vector<std::pair<std::string, std::string>> moves;
    for (const auto& [key, value] : data.items())
      if (const auto renamed = changes.renamedComponents.find(key); renamed != changes.renamedComponents.end())
        moves.emplace_back(key, renamed->second);
    for (const auto& [from, to] : moves)
    {
      if (!data.contains(to))
        data[to] = std::move(data[from]);
      data.erase(from);
      changed = true;
    }
    // A scene's game systems, by name.
    if (const auto systems = data.find("systems"); systems != data.end() && systems->is_array())
      for (json& name : *systems)
        if (name.is_string())
          if (const auto renamed = changes.renamedSystems.find(name.get<std::string>());
              renamed != changes.renamedSystems.end())
          {
            name = renamed->second;
            changed = true;
          }

    for (auto& [key, value] : data.items())
    {
      if (value.is_object())
      {
        if (const auto component = changes.components.find(key); component != changes.components.end())
          changed |= rewrite(value, component->second);
        if (key == "ScriptComponent" && value.contains("script") && value["script"].is_string())
          if (const auto renamed = changes.renamedScripts.find(value["script"].get<std::string>());
              renamed != changes.renamedScripts.end())
          {
            value["script"] = renamed->second;
            changed = true;
          }
        if (key == "ScriptComponent" && value.contains("fields"))
        {
          const json& script = value.contains("script") ? value["script"] : json();
          if (script.is_string())
          {
            if (const auto type = changes.scripts.find(script.get<std::string>()); type != changes.scripts.end())
              changed |= rewrite(value["fields"], type->second);
          }
          else
            changed |= rewrite(value["fields"], unnamedScript);
        }
      }
      changed |= walk(value, changes, unnamedScript);
    }
  }
  else if (data.is_array())
    for (json& item : data)
      changed |= walk(item, changes, unnamedScript);
  return changed;
}
} // namespace

std::vector<Layout> layoutsOf(const sfs::reflect::TypeRegistry& registry, std::string_view module)
{
  std::vector<Layout> out;
  for (const sfs::reflect::TypeInfo& type : registry.types())
  {
    if (type.module != module)
      continue;
    Layout layout{type.name, type.kind, {}};
    for (const sfs::reflect::FieldInfo& field : type.fields)
      layout.fields.push_back({field.name, field.kind});
    out.push_back(std::move(layout));
  }
  return out;
}

Changes diff(const std::vector<Layout>& before, const std::vector<Layout>& after)
{
  Changes changes;
  const auto find = [](const std::vector<Layout>& layouts, const std::string& name) -> const Layout*
  {
    for (const Layout& layout : layouts)
      if (layout.type == name)
        return &layout;
    return nullptr;
  };
  const auto sameFields = [](const Layout& a, const Layout& b)
  {
    if (a.fields.size() != b.fields.size())
      return false;
    for (std::size_t i = 0; i < a.fields.size(); ++i)
      if (a.fields[i].name != b.fields[i].name || a.fields[i].kind != b.fields[i].kind)
        return false;
    return true;
  };
  const auto record = [&](const Layout& was, const Layout& now)
  {
    TypeChanges type = compare(was, now);
    if (type.renamed.empty() && type.removed.empty())
      return;
    if (now.kind == sfs::reflect::TypeKind::Component)
      changes.components[now.type] = std::move(type);
    else if (now.kind == sfs::reflect::TypeKind::GameObject)
      changes.scripts[now.type] = std::move(type);
    else if (now.kind == sfs::reflect::TypeKind::ScriptableObject)
      changes.scriptableObjects[now.type] = std::move(type);
  };

  // Types under the same name.
  for (const Layout& now : after)
    if (const Layout* was = find(before, now.type); was && was->kind == now.kind)
      record(*was, now);

  // Types gone and types new: renamed, where they pair up.
  std::vector<const Layout*> gone, added;
  for (const Layout& was : before)
    if (!find(after, was.type))
      gone.push_back(&was);
  for (const Layout& now : after)
    if (!find(before, now.type))
      added.push_back(&now);
  const auto rename = [&](const Layout& was, const Layout& now)
  {
    switch (now.kind)
    {
    case sfs::reflect::TypeKind::Component:
      changes.renamedComponents[was.type] = now.type;
      break;
    case sfs::reflect::TypeKind::GameObject:
      changes.renamedScripts[was.type] = now.type;
      break;
    case sfs::reflect::TypeKind::System:
      changes.renamedSystems[was.type] = now.type;
      break;
    case sfs::reflect::TypeKind::ScriptableObject:
      changes.renamedScriptableObjects[was.type] = now.type;
      break;
    default:
      return;
    }
    record(was, now);
  };
  // The same fields: the one type with them (of its kind) is it.
  for (const Layout*& was : gone)
  {
    const Layout* match = nullptr;
    int matches = 0;
    for (const Layout* now : added)
      if (now && now->kind == was->kind && sameFields(*was, *now))
      {
        match = now;
        ++matches;
      }
    if (matches == 1)
    {
      rename(*was, *match);
      std::erase(added, match);
      was = nullptr;
    }
  }
  std::erase(gone, nullptr);
  // Otherwise the only one gone and the only one new, of a kind.
  for (const Layout* was : gone)
  {
    const auto sameKind = [&](const Layout* layout) { return layout->kind == was->kind; };
    if (std::count_if(gone.begin(), gone.end(), sameKind) == 1 &&
        std::count_if(added.begin(), added.end(), sameKind) == 1)
      rename(*was, **std::find_if(added.begin(), added.end(), sameKind));
  }

  for (const Layout& now : after)
    if (now.kind == sfs::reflect::TypeKind::GameObject)
      for (const Field& field : now.fields)
        changes.scriptFieldsNow.insert(field.name);
  return changes;
}

std::string Changes::describe() const
{
  std::string out;
  for (const auto* renamed : {&renamedComponents, &renamedScripts, &renamedSystems, &renamedScriptableObjects})
    for (const auto& [from, to] : *renamed)
      out += (out.empty() ? "" : ", ") + from + " -> " + to;
  const auto add = [&](const std::map<std::string, TypeChanges>& types)
  {
    for (const auto& [type, changes] : types)
    {
      for (const auto& [from, to] : changes.renamed)
        out += (out.empty() ? "" : ", ") + type + "." + from + " -> " + to;
      for (const std::string& name : changes.removed)
        out += (out.empty() ? "" : ", ") + type + "." + name + " removed";
    }
  };
  add(components);
  add(scripts);
  add(scriptableObjects);
  return out;
}

bool applyToScriptableObject(const Changes& changes, nlohmann::json& contents)
{
  if (!contents.is_object() || !contents.contains("type") || !contents["type"].is_string())
    return false;
  bool changed = false;
  std::string type = contents["type"].get<std::string>();
  if (const auto renamed = changes.renamedScriptableObjects.find(type); renamed != changes.renamedScriptableObjects.end())
  {
    type = renamed->second;
    contents["type"] = type;
    changed = true;
  }
  if (const auto fields = changes.scriptableObjects.find(type);
      fields != changes.scriptableObjects.end() && contents.contains("fields"))
    changed |= rewrite(contents["fields"], fields->second);
  return changed;
}

bool apply(const Changes& changes, nlohmann::json& data)
{
  if (changes.empty())
    return false;
  return walk(data, changes, unambiguousScriptChanges(changes));
}

} // namespace fieldMigration
