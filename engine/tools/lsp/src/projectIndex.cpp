#include "projectIndex.h"

#include "lintSupport.h"

#include <sfs/codegen/parser.h>

#include <tree_sitter/api.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

namespace sfs::lsp
{

namespace
{

bool isType(TSNode node, const char* type)
{
  return !ts_node_is_null(node) && std::strcmp(ts_node_type(node), type) == 0;
}

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

} // namespace

void findQueryAliases(const TSTree* tree,
                      std::string_view source,
                      const std::filesystem::path& path,
                      std::vector<QueryAlias>& out)
{
  if (!tree)
    return;
  const TSNode root = ts_tree_root_node(tree);
  constexpr std::string_view marker = "Query";
  for (std::size_t at = source.find(marker); at != std::string_view::npos;
       at = source.find(marker, at + marker.size()))
  {
    const auto byte = static_cast<uint32_t>(at);
    // Query's name node, then up to its alias: template_type, perhaps
    // qualified_identifier, type_descriptor, alias_declaration.
    TSNode node = ts_node_descendant_for_byte_range(
        root, byte, byte + static_cast<uint32_t>(marker.size()));
    if (!isType(node, "type_identifier") ||
        codegen::nodeText(source, node) != marker)
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

void ProjectIndex::start(const std::filesystem::path& root)
{
  std::error_code error;
  m_enabled =
      !root.empty() && std::filesystem::exists(root / "project.sfsproj", error);
  m_entries.clear();
  m_aliases.clear();
  m_known.clear();
  m_pending.clear();
  if (!m_enabled)
    return;
  m_scripts = (root / "scripts").lexically_normal();
  m_known = listHeaders();
  m_pending = m_known;
}

std::vector<std::filesystem::path> ProjectIndex::listHeaders() const
{
  std::vector<std::filesystem::path> headers;
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator it(m_scripts, error), end;
       !error && it != end;
       it.increment(error))
    if (it->is_regular_file(error) && it->path().extension() == ".h")
      headers.push_back(it->path().lexically_normal());
  std::sort(headers.begin(), headers.end());
  return headers;
}

void ProjectIndex::indexSlice(TSParser* parser, std::size_t count)
{
  while (count-- > 0 && !m_pending.empty())
  {
    const std::filesystem::path header = std::move(m_pending.back());
    m_pending.pop_back();
    const std::string source = codegen::readFile(header);
    const std::unique_ptr<TSTree, decltype(&ts_tree_delete)> tree(
        ts_parser_parse_string(parser,
                               nullptr,
                               source.data(),
                               static_cast<uint32_t>(source.size())),
        &ts_tree_delete);
    replace(header,
            codegen::parseHeader(source, header, tree.get()),
            tree.get(),
            source);
  }
}

void ProjectIndex::indexAll(TSParser* parser)
{
  indexSlice(parser, m_pending.size());
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

void ProjectIndex::rescan()
{
  if (!m_enabled)
    return;
  std::vector<std::filesystem::path> now = listHeaders();
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
  for (const std::filesystem::path& header : removed)
  {
    std::erase_if(m_entries,
                  [&](const IndexEntry& entry)
                  { return entry.header == header; });
    std::erase_if(m_aliases,
                  [&](const QueryAlias& alias)
                  { return alias.header == header; });
    std::erase(m_pending, header);
  }
  m_pending.insert(m_pending.end(), added.begin(), added.end());
  m_known = std::move(now);
}

void ProjectIndex::replace(const std::filesystem::path& header,
                           const codegen::ParseResult& result,
                           const TSTree* tree,
                           std::string_view source)
{
  std::erase_if(m_aliases,
                [&](const QueryAlias& alias)
                { return alias.header == header; });
  findQueryAliases(tree, source, header, m_aliases);
  std::erase_if(m_entries,
                [&](const IndexEntry& entry)
                { return entry.header == header; });
  auto at = std::lower_bound(
      m_entries.begin(),
      m_entries.end(),
      header,
      [](const IndexEntry& entry, const std::filesystem::path& path)
      { return entry.header < path; });
  if (result.type)
    at = std::next(
        m_entries.insert(at, {header, result.type->name, result.type->kind}));
  for (const codegen::TypeModel& type : result.components)
    at = std::next(m_entries.insert(at, {header, type.name, type.kind}));
}

bool ProjectIndex::isSystem(std::string_view name) const
{
  return std::any_of(m_entries.begin(),
                     m_entries.end(),
                     [&](const IndexEntry& entry) {
                       return entry.kind == codegen::TypeKind::System &&
                              entry.name == name;
                     });
}

const QueryAlias* ProjectIndex::findAlias(std::string_view owner,
                                          std::string_view name) const
{
  const QueryAlias* outer = nullptr;
  for (const QueryAlias& alias : m_aliases)
    if (alias.name == name)
    {
      if (alias.owner == owner)
        return &alias;
      if (alias.owner.empty() && !outer)
        outer = &alias;
    }
  return outer;
}

std::filesystem::path ProjectIndex::headerOf(std::string_view name) const
{
  for (const IndexEntry& entry : m_entries)
    if (entry.name == name)
      return entry.header;
  return {};
}

bool ProjectIndex::isScriptHeader(const std::filesystem::path& path) const
{
  if (!m_enabled || path.extension() != ".h")
    return false;
  const std::string scripts = m_scripts.generic_string() + "/";
  return path.lexically_normal().generic_string().starts_with(scripts);
}

} // namespace sfs::lsp
