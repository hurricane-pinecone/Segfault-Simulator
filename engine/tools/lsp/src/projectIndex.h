#pragma once

#include "sfs/codegen/model.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct TSNode;
struct TSParser;
struct TSTree;

namespace sfs::lsp
{

/** One type a project header declares, as codegen classifies it. */
struct IndexEntry
{
  /** The header it's declared in. */
  std::filesystem::path header;
  /** Its unqualified name, e.g. "SpawnSystem". */
  std::string name;
  /** Its name with its namespaces, e.g. "game::SpawnSystem". */
  std::string qualifiedName;
  /** Component, System, GameObject... */
  codegen::TypeKind kind = codegen::TypeKind::Component;
  /** For a Derived type, its base's unqualified name; else empty. */
  std::string base;
  /** The line it's declared on, 1-based. */
  unsigned line = 0;
  /** Whether it's marked [[sfs::component]] or [[sfs::scene_data]]. */
  bool marked = false;
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
  /** Its name, e.g. "DoorParts". */
  std::string name;
  /** Its terms as written, whitespace-tidied, e.g. "sfs::Any<A, B>". */
  std::vector<std::string> terms;
};

/**
 * A response a project script sends: through a responder
 * (`responder.respond(R{...})`, `.progress(...)`, `.fail(...)`) or a
 * request consumer by entity (`consumer.respond(entity, R{...})`), for
 * response-sent-twice.
 */
struct ResponseSend
{
  /** The file it's written in. */
  std::filesystem::path file;
  /**
   * The class it's written in (unqualified); empty in a free function. A job
   * or a free function sends for the one system of its file, if it has one.
   */
  std::string owner;
  /** The type sent as written, normalised, e.g. "nav::PathFoundResponse". */
  std::string spelling;
  /**
   * The namespaces the call is in, outermost first: those around it, then an
   * out-of-line definition's qualifier (`void nav::NavSystem::update()`).
   */
  std::vector<std::string> scopes;
  /** Where the call is written. */
  codegen::SourceRange range;
  /** The call's first byte, to tell which using-declarations it sees. */
  uint32_t offset = 0;
};

/**
 * A qualified name a response type of a project script may have: a struct
 * marked `[[sfs::event_response]]` declares its own; a type a
 * `[[sfs::event_request(...)]]` lists has each name the request's namespaces
 * let it mean.
 */
struct ResponseType
{
  /** The file it's declared or listed in. */
  std::filesystem::path file;
  /** The name with its namespaces, e.g. "nav::PathFoundResponse". */
  std::string qualifiedName;
  /** Whether it's declared (marked), not only listed. */
  bool declared = false;
};

/** A name a file brings into scope (codegen's, which codegen reads too). */
using UsingName = codegen::UsingName;

/** How a qualified name is known as a response type. */
enum class ResponseKind : uint8_t
{
  /** Not a response type of the project's scripts. */
  None,
  /** A request lists it, but no script declares it (an engine type). */
  Listed,
  /** A script declares it, marked `[[sfs::event_response]]`. */
  Declared,
};

/**
 * Adds the response types codegen's event models of a file give.
 *
 * @param events parseHeader()'s events for the file
 * @param path the file
 * @param types gains each name, in the order declared
 */
void responseTypesOf(const std::vector<codegen::EventModel>& events,
                     const std::filesystem::path& path,
                     std::vector<ResponseType>& types);

/**
 * Adds a response a call sends, if it's a responder's `x.respond(r)`,
 * `x.progress(r)` or `x.fail(r)`, or a request consumer's by entity,
 * `x.respond(entity, r)` and the like, where r's type can be read: `R{...}`,
 * `R(...)`, or a variable the function around the call declares (not `auto`
 * but for `auto v = R{...}`), or a data member the class around it declares.
 *
 * @param call a call expression
 * @param source the file's text
 * @param path the file
 * @param sends gains the response, if it's one
 */
void noteResponseSend(TSNode call,
                      std::string_view source,
                      const std::filesystem::path& path,
                      std::vector<ResponseSend>& sends);

/**
 * Adds the name a using-declaration, using-directive or alias brings in.
 *
 * @param node a using or alias declaration
 * @param source the file's text
 * @param path the file
 * @param usings gains the name, if it's one of those
 */
void noteUsing(TSNode node,
               std::string_view source,
               const std::filesystem::path& path,
               std::vector<UsingName>& usings);

/**
 * The type of the value a send-like call sends (`x.respond(r)`,
 * `send(entity, e)`): its one argument's, or the second of two, where it can
 * be read: `T{...}`, `T(...)`, a variable the function around the call
 * declares (not `auto` but for `auto v = T{...}`), or a data member the class
 * around it declares, through `std::move`.
 *
 * @param call a call expression
 * @param source the file's text
 * @return the type as written, or empty when it can't be read
 */
std::string_view sentType(TSNode call, std::string_view source);

/**
 * Finds the responses a file sends and the names it brings into scope, with
 * noteResponseSend() and noteUsing().
 *
 * @param tree the file's syntax tree
 * @param source the file's text
 * @param path the file
 * @param sends gains each response, in the order written
 * @param usings gains each using name, in the order written
 */
void findResponses(const TSTree* tree,
                   std::string_view source,
                   const std::filesystem::path& path,
                   std::vector<ResponseSend>& sends,
                   std::vector<UsingName>& usings);

/**
 * The response type a send sends: its spelling looked up from where it's
 * written (codegen's lookUpName(), as event declarations are), preferring a
 * declared response type to one only listed.
 *
 * @param send the send
 * @param usings its file's using names
 * @param kindOf how a qualified name is known as a response type
 * @return the qualified name, or empty when it's no response type
 */
std::string
resolveResponse(const ResponseSend& send,
                std::span<const UsingName> usings,
                const std::function<ResponseKind(std::string_view)>& kindOf);

/** What the index holds of one script file. */
struct ScriptFile
{
  /** The types codegen registers from it (a header's), in order. */
  std::vector<IndexEntry> entries;
  /** The event types it declares, in order. */
  std::vector<codegen::EventModel> events;
  /** Its query aliases (a header's). */
  std::vector<QueryAlias> aliases;
  /** The responses it sends. */
  std::vector<ResponseSend> sends;
  /** The response type names its events give. */
  std::vector<ResponseType> responseTypes;
  /** The names it brings into scope. */
  std::vector<UsingName> usings;
};

/** One indexed send of a response type, by the system it's sent for. */
struct ResponseSender
{
  /** The file the send is written in. */
  const std::filesystem::path* file = nullptr;
  /** The system or service it's sent for. */
  std::string_view system;
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
 * What the project's scripts (every .h and .cpp under <root>/scripts) declare
 * and do: the types codegen registers (from the headers), so a lint can tell
 * the project's systems in any file, and the responses each class sends and
 * the response types (from headers and sources), so a lint can tell a
 * response's other senders. Built in slices while the server is idle, and
 * kept current as files are opened, saved, added and removed. A root without
 * project.sfsproj has no index.
 */
class ProjectIndex
{
public:
  /**
   * Adds files to be read by indexSlice() besides the project's scripts (or
   * in place of them, for a root that isn't a project), and puts the index in
   * use: sfs-lsp --graph on any tree. A later rescan() forgets them.
   *
   * @param files the .h and .cpp files, each once
   */
  void addFiles(std::span<const std::filesystem::path> files);

  /** Whether the root is a project, so the index is in use. */
  bool enabled() const { return m_enabled; }

  /**
   * The script files read, in path order, each with what it declares and
   * sends.
   */
  std::span<const ScriptFile* const> files() const;

  /**
   * The type `name` (unqualified) a script header declares, as codegen
   * registers it; null when there's none.
   */
  const IndexEntry* find(std::string_view name) const;

  /**
   * The query alias `name` declared in class `owner` of a script header, else
   * at namespace scope; null when there's none.
   */
  const QueryAlias* findAlias(std::string_view owner,
                              std::string_view name) const;

  /**
   * The event type a script file declares as `name`: by its qualified name
   * ("game::DoorOpenedEvent"), else by its unqualified one; null when there's
   * none.
   */
  const codegen::EventModel* findEvent(std::string_view name) const;

  /**
   * The script header that declares the type `name` (unqualified), or empty.
   */
  std::filesystem::path headerOf(std::string_view name) const;

  /** Reads every waiting script file (sfs-lsp --check). */
  void indexAll(TSParser* parser);

  /**
   * Reads one file from a tree the caller has, as indexSlice() reads one
   * from disk, and takes it off the waiting list (sfs-lsp --graph, which
   * parses each file once).
   *
   * @param file the .h or .cpp file
   * @param tree its syntax tree
   * @param source its text
   */
  void indexFile(const std::filesystem::path& file,
                 const TSTree* tree,
                 std::string_view source);

  /**
   * Reads up to `count` waiting script files from disk.
   *
   * @param parser a C++ tree-sitter parser to parse them with
   * @param count how many to read at most
   */
  void indexSlice(TSParser* parser, std::size_t count);

  /**
   * Whether `name` (unqualified) is one of the project's game objects:
   * deriving from sfs::GameObject, or from another of its game objects.
   */
  bool isGameObject(std::string_view name) const;

  /** Whether `path` is a script header of the project (codegen reads it). */
  bool isScriptHeader(const std::filesystem::path& path) const;

  /** Whether `path` is a script source (.cpp) of the project. */
  bool isScriptSource(const std::filesystem::path& path) const;

  /**
   * Whether `name` (unqualified) is one of the project's services: deriving
   * from sfs::Service, or from another of the project's services.
   */
  bool isService(std::string_view name) const;

  /**
   * Whether `name` (unqualified) is one of the project's systems: deriving
   * from sfs::System, or from another of the project's systems.
   */
  bool isSystem(std::string_view name) const;

  /** Whether script files are still waiting to be read. */
  bool pending() const { return !m_pending.empty(); }

  /** How many script files are waiting to be read. */
  std::size_t pendingCount() const { return m_pending.size(); }

  /**
   * Looks for script files added or removed since the last look (one stat
   * each).
   */
  void rescan();

  /**
   * How `qualifiedName` (e.g. "nav::PathFoundResponse") is known as a
   * response type of the project's scripts.
   */
  ResponseKind responseKind(std::string_view qualifiedName) const;

  /**
   * The kind of the type `name` (unqualified) the scripts declare: a Derived
   * type's is its base's, as far as the scripts declare its bases (Derived
   * when they don't); Derived for a name they don't declare.
   */
  codegen::TypeKind rootKind(std::string_view name) const;

  /**
   * The project's sends of the response type `qualifiedName`, each resolved
   * and given the system it's sent for once, as the index changes; sends
   * that are nobody's are left out.
   */
  std::span<const ResponseSender>
  sendersOf(std::string_view qualifiedName) const;

  /**
   * Starts indexing a root: lists its script files (headers and sources), to
   * be read by indexSlice(). Does nothing for a root that isn't an SFS
   * project.
   *
   * @param root the workspace root
   */
  void start(const std::filesystem::path& root);

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
   * Refreshes one source file's responses and response types (an open
   * buffer's), if it's a script source of the project.
   *
   * @param file the .cpp file
   * @param tree its syntax tree
   * @param source its text
   */
  void updateSource(const std::filesystem::path& file,
                    const TSTree* tree,
                    std::string_view source);

private:
  // Hashes std::string and std::string_view alike, so a map keyed by
  // std::string is searched with a view.
  struct NameHash
  {
    using is_transparent = void;

    std::size_t operator()(std::string_view name) const;
  };
  template <typename T>
  using NameMap = std::unordered_map<std::string, T, NameHash, std::equal_to<>>;

  void buildLookup() const;
  ScriptFile& fileAt(const std::filesystem::path& normal);
  bool isScriptFile(const std::filesystem::path& path,
                    std::string_view extension) const;
  std::vector<std::filesystem::path> listFiles() const;
  void readTree(const std::filesystem::path& normal,
                const TSTree* tree,
                std::string_view source);
  void replace(const std::filesystem::path& header,
               const codegen::ParseResult& result,
               const TSTree* tree,
               std::string_view source);
  void replaceResponses(ScriptFile& file,
                        const std::filesystem::path& path,
                        const TSTree* tree,
                        std::string_view source,
                        const std::vector<codegen::EventModel>* events);

  bool m_enabled = false;
  std::filesystem::path m_scripts;
  // Each script file's data, by its normalised path: re-reading a file
  // replaces its own entry only.
  NameMap<ScriptFile> m_files;

  // What's derived from the files, built when first needed after a change
  // (buildLookup()): the files in path order, the entries and events by
  // name, the response types, and the resolved senders by response type.
  mutable bool m_lookupBuilt = false;
  mutable std::vector<const ScriptFile*> m_sorted;
  mutable NameMap<const IndexEntry*> m_byName;
  mutable NameMap<const codegen::EventModel*> m_eventsByName;
  mutable NameMap<const codegen::EventModel*> m_eventsByQualifiedName;
  mutable NameMap<ResponseKind> m_responseKinds;
  mutable NameMap<std::vector<ResponseSender>> m_senders;
  std::vector<std::filesystem::path> m_known;   // sorted: every file listed
  std::vector<std::filesystem::path> m_pending; // sorted: waiting to be read
};

} // namespace sfs::lsp
