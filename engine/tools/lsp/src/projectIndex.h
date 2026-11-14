#pragma once

#include <sfs/codegen/model.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct TSParser;
struct TSTree;

namespace sfs::lsp
{

/** One type a project header declares, as codegen classifies it. */
struct IndexEntry
{
  /** The header it's declared in. */
  std::filesystem::path header;
  /** Its unqualified name, e.g. "FieldSystem". */
  std::string name;
  /** Component, System, GameObject... */
  codegen::TypeKind kind = codegen::TypeKind::Component;
};

/**
 * A named list of query terms, `using Name = sfs::Query<...>;`, as written:
 * what a term list naming it stands for.
 */
struct QueryAlias
{
  /** The file it's declared in. */
  std::filesystem::path header;
  /** The class it's declared in (unqualified); empty at namespace scope. */
  std::string owner;
  /** Its name, e.g. "HitchVehicles". */
  std::string name;
  /** Its terms as written, whitespace-tidied, e.g. "sfs::Any<A, B>". */
  std::vector<std::string> terms;
};

/**
 * Finds a file's query aliases (`using X = sfs::Query<...>;`, or `Query<...>`
 * where SFS's is visible).
 *
 * @param tree the file's syntax tree
 * @param source the file's text
 * @param path the file, for QueryAlias::header
 * @param out gains each alias, in the order written
 */
void findQueryAliases(const TSTree* tree,
                      std::string_view source,
                      const std::filesystem::path& path,
                      std::vector<QueryAlias>& out);

/**
 * What the project's script headers (every .h under <root>/scripts) declare:
 * the types codegen registers, so a lint can tell the project's systems in any
 * file. Built in slices while the server is idle, and kept current as headers
 * are opened, saved, added and removed. A root without project.sfsproj has no
 * index.
 */
class ProjectIndex
{
public:
  /**
   * Starts indexing a root: lists its script headers, to be read by
   * indexSlice(). Does nothing for a root that isn't an SFS project.
   *
   * @param root the workspace root
   */
  void start(const std::filesystem::path& root);

  /** Whether the root is a project, so the index is in use. */
  bool enabled() const { return m_enabled; }

  /** Whether headers are still waiting to be read. */
  bool pending() const { return !m_pending.empty(); }

  /**
   * Reads up to `count` waiting headers from disk.
   *
   * @param parser a C++ tree-sitter parser to parse them with
   * @param count how many to read at most
   */
  void indexSlice(TSParser* parser, std::size_t count);

  /** Reads every waiting header (sfs-lsp --check). */
  void indexAll(TSParser* parser);

  /**
   * Refreshes one header's entries from codegen's reading of it (an open
   * buffer's), if it's a script header of the project.
   *
   * @param header the header
   * @param parsed parseHeader()'s result for it
   * @param tree its syntax tree, for its query aliases
   * @param source its text
   */
  void update(const std::filesystem::path& header,
              const codegen::ParseResult& parsed,
              const TSTree* tree,
              std::string_view source);

  /**
   * Looks for script headers added or removed since the last look (one stat
   * each).
   */
  void rescan();

  /** Whether `name` (unqualified) is one of the project's systems. */
  bool isSystem(std::string_view name) const;

  /** Whether `path` is a script header of the project (codegen reads it). */
  bool isScriptHeader(const std::filesystem::path& path) const;

  /**
   * The query alias `name` declared in class `owner` of a script header, else
   * at namespace scope; null when there's none.
   */
  const QueryAlias* findAlias(std::string_view owner,
                              std::string_view name) const;

  /**
   * The script header that declares the type `name` (unqualified), or empty.
   */
  std::filesystem::path headerOf(std::string_view name) const;

  /** The entries, sorted by header. */
  const std::vector<IndexEntry>& entries() const { return m_entries; }

private:
  void replace(const std::filesystem::path& header,
               const codegen::ParseResult& result,
               const TSTree* tree,
               std::string_view source);
  std::vector<std::filesystem::path> listHeaders() const;

  bool m_enabled = false;
  std::filesystem::path m_scripts;
  std::vector<IndexEntry> m_entries;
  std::vector<QueryAlias> m_aliases;            // every script header's
  std::vector<std::filesystem::path> m_known;   // sorted: every header listed
  std::vector<std::filesystem::path> m_pending; // waiting to be read
};

} // namespace sfs::lsp
