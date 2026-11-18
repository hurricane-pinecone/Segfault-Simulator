#include "projectIndex.h"

#include "lintSupport.h"
#include "sfs/codegen/generator.h"
#include "sfs/codegen/model.h"
#include "sfs/codegen/parser.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace sfs::lsp
{

namespace
{

using lint::isType;

// Whether SFS's unqualified names are visible at `node`: inside namespace sfs,
// or anywhere in a file that says `using namespace sfs;` or `using
// sfs::Query;`.
bool sfsVisible(TSNode node, std::string_view source)
{
  for (TSNode at = node; !ts_node_is_null(at); at = ts_node_parent(at))
    if (isType(at, "namespace_definition") &&
        codegen::isSfsNamespace(
            codegen::nodeText(source, codegen::childField(at, "name"))))
      return true;

  return source.find("using namespace sfs;") != std::string_view::npos ||
         source.find("using sfs::Query;") != std::string_view::npos;
}

// The type a data member `name` has, as the class around `at` declares it;
// empty when it isn't written there.
std::string_view
memberTypeOf(TSNode at, std::string_view name, std::string_view source)
{
  for (TSNode up = lint::parent(at); !ts_node_is_null(up);
       up = lint::parent(up))
    if (isType(up, "class_specifier") || isType(up, "struct_specifier"))
    {
      const TSNode member = lint::memberDeclaration(up, name, source);
      return ts_node_is_null(member)
                 ? std::string_view{}
                 : lint::trimmed(codegen::nodeText(
                       source, codegen::childField(member, "type")));
    }

  return {};
}

} // namespace

void ProjectIndex::addFiles(std::span<const std::filesystem::path> files)
{
  m_enabled = true;

  for (const std::filesystem::path& file : files)
  {
    const std::filesystem::path normal = file.lexically_normal();
    const auto at = std::lower_bound(m_known.begin(), m_known.end(), normal);

    if (at != m_known.end() && *at == normal)
      continue;

    m_known.insert(at, normal);
    m_pending.insert(
        std::lower_bound(m_pending.begin(), m_pending.end(), normal), normal);
  }
}

void ProjectIndex::buildLookup() const
{
  if (m_lookupBuilt)
    return;

  m_lookupBuilt = true;
  // The files in path order, so the first of two types of one name is
  // always the same one.
  std::vector<const std::pair<const std::string, ScriptFile>*> ordered;
  ordered.reserve(m_files.size());

  for (const auto& file : m_files)
    ordered.push_back(&file);

  std::sort(ordered.begin(),
            ordered.end(),
            [](const auto* a, const auto* b) { return a->first < b->first; });
  m_sorted.clear();
  m_sorted.reserve(ordered.size());

  for (const auto* file : ordered)
    m_sorted.push_back(&file->second);

  m_byName.clear();
  m_eventsByName.clear();
  m_eventsByQualifiedName.clear();
  m_responseKinds.clear();
  m_senders.clear();

  for (const ScriptFile* file : m_sorted)
  {
    for (const IndexEntry& entry : file->entries)
      m_byName.emplace(entry.name, &entry);

    for (const codegen::EventModel& event : file->events)
    {
      m_eventsByName.emplace(event.name, &event);
      m_eventsByQualifiedName.emplace(event.qualifiedName, &event);
    }
  }

  for (const ScriptFile* file : m_sorted)
    for (const ResponseType& type : file->responseTypes)
    {
      ResponseKind& kind = m_responseKinds[type.qualifiedName];

      if (type.declared)
        kind = ResponseKind::Declared;
      else if (kind == ResponseKind::None)
        kind = ResponseKind::Listed;
    }

  const auto kindOf = [&](std::string_view name)
  {
    const auto found = m_responseKinds.find(name);
    return found == m_responseKinds.end() ? ResponseKind::None : found->second;
  };

  for (const ScriptFile* file : m_sorted)
  {
    // The one system or service the file sends for, when it has one: its
    // jobs' and free functions'.
    const auto sends = [&](std::string_view name)
    { return isSystem(name) || isService(name); };
    std::string_view only;
    bool several = false;
    const auto consider = [&](std::string_view name)
    {
      if (only.empty())
        only = name;
      else if (only != name)
        several = true;
    };

    for (const IndexEntry& entry : file->entries)
      if (sends(entry.name))
        consider(entry.name);

    for (const ResponseSend& send : file->sends)
      if (sends(send.owner))
        consider(send.owner);

    for (const ResponseSend& send : file->sends)
    {
      const std::string_view system = sends(send.owner)
                                          ? std::string_view(send.owner)
                                      : several ? std::string_view{}
                                                : only;

      if (system.empty())
        continue;

      std::string type = resolveResponse(send, file->usings, kindOf);

      if (!type.empty())
        m_senders[std::move(type)].push_back({&send.file, system});
    }
  }
}

ScriptFile& ProjectIndex::fileAt(const std::filesystem::path& normal)
{
  m_lookupBuilt = false;
  return m_files[normal.generic_string()];
}

std::span<const ScriptFile* const> ProjectIndex::files() const
{
  buildLookup();
  return m_sorted;
}

const IndexEntry* ProjectIndex::find(std::string_view name) const
{
  buildLookup();
  const auto found = m_byName.find(name);
  return found == m_byName.end() ? nullptr : found->second;
}

const QueryAlias* ProjectIndex::findAlias(std::string_view owner,
                                          std::string_view name) const
{
  buildLookup();
  const QueryAlias* outer = nullptr;

  for (const ScriptFile* file : m_sorted)
    for (const QueryAlias& alias : file->aliases)
      if (alias.name == name)
      {
        if (alias.owner == owner)
          return &alias;

        if (alias.owner.empty() && !outer)
          outer = &alias;
      }

  return outer;
}

const codegen::EventModel* ProjectIndex::findEvent(std::string_view name) const
{
  buildLookup();

  if (const auto found = m_eventsByQualifiedName.find(name);
      found != m_eventsByQualifiedName.end())
    return found->second;

  const auto found = m_eventsByName.find(name);
  return found == m_eventsByName.end() ? nullptr : found->second;
}

std::filesystem::path ProjectIndex::headerOf(std::string_view name) const
{
  const IndexEntry* entry = find(name);
  return entry ? entry->header : std::filesystem::path{};
}

void ProjectIndex::indexAll(TSParser* parser)
{
  indexSlice(parser, m_pending.size());
}

void ProjectIndex::indexFile(const std::filesystem::path& file,
                             const TSTree* tree,
                             std::string_view source)
{
  const std::filesystem::path normal = file.lexically_normal();

  if (const auto at =
          std::lower_bound(m_pending.begin(), m_pending.end(), normal);
      at != m_pending.end() && *at == normal)
    m_pending.erase(at);

  readTree(normal, tree, source);
}

void ProjectIndex::indexSlice(TSParser* parser, std::size_t count)
{
  while (count-- > 0 && !m_pending.empty())
  {
    const std::filesystem::path file = std::move(m_pending.back());
    m_pending.pop_back();
    const std::string source = codegen::readFile(file);
    const std::unique_ptr<TSTree, decltype(&ts_tree_delete)> tree(
        ts_parser_parse_string(parser,
                               nullptr,
                               source.data(),
                               static_cast<uint32_t>(source.size())),
        &ts_tree_delete);

    readTree(file, tree.get(), source);
  }
}

bool ProjectIndex::isGameObject(std::string_view name) const
{
  return !name.empty() && rootKind(name) == codegen::TypeKind::GameObject;
}

bool ProjectIndex::isScriptFile(const std::filesystem::path& path,
                                std::string_view extension) const
{
  if (!m_enabled || m_scripts.empty() || path.extension() != extension)
    return false;

  const std::string scripts = m_scripts.generic_string() + "/";
  return path.lexically_normal().generic_string().starts_with(scripts);
}

bool ProjectIndex::isScriptHeader(const std::filesystem::path& path) const
{
  return isScriptFile(path, ".h");
}

bool ProjectIndex::isScriptSource(const std::filesystem::path& path) const
{
  return isScriptFile(path, ".cpp");
}

bool ProjectIndex::isService(std::string_view name) const
{
  return !name.empty() && rootKind(name) == codegen::TypeKind::Service;
}

bool ProjectIndex::isSystem(std::string_view name) const
{
  return !name.empty() && rootKind(name) == codegen::TypeKind::System;
}

std::vector<std::filesystem::path> ProjectIndex::listFiles() const
{
  std::vector<std::filesystem::path> files;
  std::error_code error;

  for (std::filesystem::recursive_directory_iterator it(m_scripts, error), end;
       !error && it != end;
       it.increment(error))
    if (it->is_regular_file(error) &&
        (it->path().extension() == ".h" || it->path().extension() == ".cpp"))
      files.push_back(it->path().lexically_normal());

  std::sort(files.begin(), files.end());
  return files;
}

void ProjectIndex::readTree(const std::filesystem::path& normal,
                            const TSTree* tree,
                            std::string_view source)
{
  if (normal.extension() == ".cpp")
    replaceResponses(fileAt(normal), normal, tree, source, nullptr);
  else
    replace(normal, codegen::parseHeader(source, normal, tree), tree, source);
}

void ProjectIndex::replace(const std::filesystem::path& header,
                           const codegen::ParseResult& result,
                           const TSTree* tree,
                           std::string_view source)
{
  ScriptFile& file = fileAt(header);
  file.aliases.clear();
  findQueryAliases(tree, source, header, file.aliases);
  replaceResponses(file, header, tree, source, &result.events);
  file.entries.clear();

  if (result.type)
    file.entries.push_back({header,
                            result.type->name,
                            result.type->qualifiedName,
                            result.type->kind,
                            result.type->base,
                            result.type->line,
                            result.type->marked});

  for (const codegen::TypeModel& type : result.components)
    file.entries.push_back({header,
                            type.name,
                            type.qualifiedName,
                            type.kind,
                            type.base,
                            type.line,
                            type.marked});
}

void ProjectIndex::replaceResponses(
    ScriptFile& file,
    const std::filesystem::path& path,
    const TSTree* tree,
    std::string_view source,
    const std::vector<codegen::EventModel>* events)
{
  m_lookupBuilt = false;
  file.sends.clear();
  file.responseTypes.clear();
  file.usings.clear();
  file.events.clear();
  findResponses(tree, source, path, file.sends, file.usings);

  // A source's events are read by codegen too, when it declares any.
  if (events)
    file.events = *events;
  else if (source.find("sfs::event_") != std::string_view::npos)
    file.events = codegen::parseHeader(source, path, tree).events;

  responseTypesOf(file.events, path, file.responseTypes);
}

void ProjectIndex::rescan()
{
  if (!m_enabled)
    return;

  std::vector<std::filesystem::path> now = listFiles();
  std::vector<std::filesystem::path> added;
  std::set_difference(now.begin(),
                      now.end(),
                      m_known.begin(),
                      m_known.end(),
                      std::back_inserter(added));
  std::vector<std::filesystem::path> removed;
  std::set_difference(m_known.begin(),
                      m_known.end(),
                      now.begin(),
                      now.end(),
                      std::back_inserter(removed));

  for (const std::filesystem::path& file : removed)
  {
    if (const auto found = m_files.find(file.generic_string());
        found != m_files.end())
      m_files.erase(found);

    std::erase(m_pending, file);
    m_lookupBuilt = false;
  }

  m_pending.insert(m_pending.end(), added.begin(), added.end());
  std::sort(m_pending.begin(), m_pending.end());
  m_known = std::move(now);
}

ResponseKind ProjectIndex::responseKind(std::string_view qualifiedName) const
{
  buildLookup();
  const auto found = m_responseKinds.find(qualifiedName);
  return found == m_responseKinds.end() ? ResponseKind::None : found->second;
}

codegen::TypeKind ProjectIndex::rootKind(std::string_view name) const
{
  // A Derived type is what its base is. Each type is looked at once, so a
  // cycle of bases ends the walk.
  const IndexEntry* entry = find(name);
  std::vector<const IndexEntry*> seen;

  while (entry && entry->kind == codegen::TypeKind::Derived &&
         std::find(seen.begin(), seen.end(), entry) == seen.end())
  {
    seen.push_back(entry);
    const IndexEntry* base = find(entry->base);

    if (!base)
      break;

    entry = base;
  }

  return entry ? entry->kind : codegen::TypeKind::Derived;
}

std::span<const ResponseSender>
ProjectIndex::sendersOf(std::string_view qualifiedName) const
{
  buildLookup();
  const auto found = m_senders.find(qualifiedName);
  return found == m_senders.end() ? std::span<const ResponseSender>{}
                                  : std::span(found->second);
}

void ProjectIndex::start(const std::filesystem::path& root)
{
  std::error_code error;
  m_enabled =
      !root.empty() && std::filesystem::exists(root / "project.sfsproj", error);
  m_files.clear();
  m_lookupBuilt = false;
  m_known.clear();
  m_pending.clear();

  if (!m_enabled)
    return;

  m_scripts = (root / "scripts").lexically_normal();
  m_known = listFiles();
  m_pending = m_known;
}

void ProjectIndex::update(const std::filesystem::path& header,
                          const codegen::ParseResult& parsed,
                          const TSTree* tree,
                          std::string_view source)
{
  if (!isScriptHeader(header))
    return;

  const std::filesystem::path normal = header.lexically_normal();
  std::erase(m_pending, normal);
  replace(normal, parsed, tree, source);
}

void ProjectIndex::updateSource(const std::filesystem::path& file,
                                const TSTree* tree,
                                std::string_view source)
{
  if (!isScriptSource(file))
    return;

  const std::filesystem::path normal = file.lexically_normal();
  std::erase(m_pending, normal);
  replaceResponses(fileAt(normal), normal, tree, source, nullptr);
}

std::size_t ProjectIndex::NameHash::operator()(std::string_view name) const
{
  return std::hash<std::string_view>{}(name);
}

std::string_view sentType(TSNode call, std::string_view source)
{
  const TSNode arguments = codegen::childField(call, "arguments");
  const uint32_t count = lint::namedCount(arguments);

  if (count != 1 && count != 2)
    return {};

  TSNode value = lint::namedChild(arguments, count - 1);

  if (isType(value, "call_expression"))
  {
    const std::string_view callee =
        codegen::nodeText(source, codegen::childField(value, "function"));
    const TSNode inner = codegen::childField(value, "arguments");

    if ((callee == "std::move" || callee == "move") &&
        lint::namedCount(inner) == 1)
      value = lint::namedChild(inner, 0);
  }

  if (!isType(value, "identifier"))
    return lint::madeType(value, source);

  const std::string_view name = codegen::nodeText(source, value);
  const std::string_view local = lint::localTypeOf(call, name, source);
  return local.empty() ? memberTypeOf(call, name, source) : local;
}

void responseTypesOf(const std::vector<codegen::EventModel>& events,
                     const std::filesystem::path& path,
                     std::vector<ResponseType>& types)
{
  for (const codegen::EventModel& event : events)
  {
    if (event.kind == codegen::EventKind::Response)
      types.push_back({path, event.qualifiedName, true});
    else if (event.kind == codegen::EventKind::Request)
      for (const std::string* listed :
           {&event.success, &event.progress, &event.failure})
        if (!listed->empty())
          for (std::string& name :
               codegen::lookUpName(
                   *listed, event.scopes, event.offset, event.usings)
                   .candidates)
            types.push_back({path, std::move(name), false});
  }
}

void noteResponseSend(TSNode call,
                      std::string_view source,
                      const std::filesystem::path& path,
                      std::vector<ResponseSend>& sends)
{
  TSNode templateArguments;
  const std::string_view name =
      lint::calleeName(call, source, templateArguments);

  if ((name != "respond" && name != "progress" && name != "fail") ||
      !isType(codegen::childField(call, "function"), "field_expression"))
    return;

  // respond<T>(...) names its type; otherwise it's the argument's.
  std::string_view spelled;

  if (!ts_node_is_null(templateArguments) &&
      ts_node_named_child_count(templateArguments) > 0)
    spelled =
        codegen::nodeText(source, ts_node_named_child(templateArguments, 0));

  if (spelled.empty())
    spelled = sentType(call, source);

  if (spelled.empty())
    return;

  ResponseSend send{path,
                    std::string(lint::ownerOf(call, source)),
                    codegen::normaliseType(spelled),
                    {},
                    codegen::rangeOf(call),
                    ts_node_start_byte(call)};
  lint::namespacesOf(call, source, send.scopes);
  sends.push_back(std::move(send));
}

void noteUsing(TSNode node,
               std::string_view source,
               const std::filesystem::path& path,
               std::vector<UsingName>& usings)
{
  std::vector<std::string> scopes;
  lint::namespacesOf(node, source, scopes);
  codegen::readUsingName(node, source, path, std::move(scopes), usings);
}

void findResponses(const TSTree* tree,
                   std::string_view source,
                   const std::filesystem::path& path,
                   std::vector<ResponseSend>& sends,
                   std::vector<UsingName>& usings)
{
  if (!tree)
    return;

  lint::walkTree(ts_tree_root_node(tree),
                 [&](TSNode node)
                 {
                   if (ts_node_is_error(node))
                     return lint::Next::Skip;

                   if (isType(node, "call_expression"))
                     noteResponseSend(node, source, path, sends);
                   else if (isType(node, "using_declaration") ||
                            isType(node, "alias_declaration") ||
                            isType(node, "type_definition") ||
                            isType(node, "namespace_alias_definition"))
                     noteUsing(node, source, path, usings);

                   return lint::Next::Descend;
                 });
}

std::string
resolveResponse(const ResponseSend& send,
                std::span<const UsingName> usings,
                const std::function<ResponseKind(std::string_view)>& kindOf)
{
  codegen::NameLookup lookup =
      codegen::lookUpName(send.spelling, send.scopes, send.offset, usings);

  for (const ResponseKind wanted :
       {ResponseKind::Declared, ResponseKind::Listed})
    for (std::string& name : lookup.candidates)
      if (kindOf(name) == wanted)
        return std::move(name);

  return {};
}

void findQueryAliases(const TSTree* tree,
                      std::string_view source,
                      const std::filesystem::path& path,
                      std::vector<QueryAlias>& out)
{
  if (!tree)
    return;

  const TSNode root = ts_tree_root_node(tree);
  constexpr std::string_view MARKER = "Query";

  for (std::size_t at = source.find(MARKER); at != std::string_view::npos;
       at = source.find(MARKER, at + MARKER.size()))
  {
    const auto byte = static_cast<uint32_t>(at);
    // Query's name node, then up to its alias: template_type, perhaps
    // qualified_identifier, type_descriptor, alias_declaration.
    TSNode node = ts_node_descendant_for_byte_range(
        root, byte, byte + static_cast<uint32_t>(MARKER.size()));

    if (!isType(node, "type_identifier") ||
        codegen::nodeText(source, node) != MARKER)
      continue;

    const TSNode templateType = ts_node_parent(node);

    if (!isType(templateType, "template_type"))
      continue;

    TSNode type = templateType;
    bool qualified = false;
    const TSNode parent = ts_node_parent(type);

    if (isType(parent, "qualified_identifier"))
    {
      if (codegen::nodeText(source, codegen::childField(parent, "scope")) !=
          "sfs")
        continue;

      qualified = true;
      type = parent;
    }

    const TSNode descriptor = ts_node_parent(type);
    const TSNode declaration = ts_node_parent(descriptor);

    if (!isType(descriptor, "type_descriptor") ||
        !isType(declaration, "alias_declaration") ||
        (!qualified && !sfsVisible(declaration, source)))
      continue;

    QueryAlias alias;
    alias.header = path;
    alias.name = std::string(
        codegen::nodeText(source, codegen::childField(declaration, "name")));
    const TSNode body = ts_node_parent(declaration);

    if (isType(body, "field_declaration_list"))
      alias.owner = std::string(codegen::nodeText(
          source, codegen::childField(ts_node_parent(body), "name")));

    const TSNode arguments = codegen::childField(templateType, "arguments");
    const uint32_t count =
        ts_node_is_null(arguments) ? 0 : ts_node_named_child_count(arguments);

    for (uint32_t i = 0; i < count; ++i)
      alias.terms.push_back(lint::tidy(
          codegen::nodeText(source, ts_node_named_child(arguments, i))));

    out.push_back(std::move(alias));
  }
}

} // namespace sfs::lsp
