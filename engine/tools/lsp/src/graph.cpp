// sfs-lsp --graph: a tree's architecture as JSON. The types come from the
// project index; one walk of each exported file collects its calls, using
// names and includes, and each call the SFS API knows becomes an edge from
// the system, service, game object or utils file whose code it's written in.

#include "graph.h"

#include "document.h"
#include "lints.h"
#include "lintSupport.h"
#include "projectIndex.h"
#include "server.h"
#include "sfs/codegen/generator.h"
#include "sfs/codegen/model.h"
#include "sfs/codegen/parser.h"

#include <nlohmann/json.hpp>
#include <tree_sitter/api.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace sfs::lsp
{

namespace
{

using Json = nlohmann::ordered_json;
using Kind = Linter::Kind;
using lint::lastName;
using lint::namedChild;
using lint::namedCount;
using lint::Wrapper;

/** What one node's code does with another. */
enum class EdgeKind : uint8_t
{
  Registers,
  Observes,
  Reads,
  Writes,
  Adds,
  Removes,
  Filters,
  Sends,
  Consumes,
  Requests,
  Responds,
  ReadsSystem,
  UsesUtil,
};

// The edge kinds as the JSON names them, in EdgeKind's order.
constexpr std::array<std::string_view, 13> EDGE_KINDS = {"registers",
                                                         "observes",
                                                         "reads",
                                                         "writes",
                                                         "adds",
                                                         "removes",
                                                         "filters",
                                                         "sends",
                                                         "consumes",
                                                         "requests",
                                                         "responds",
                                                         "reads-system",
                                                         "uses-util"};

/** What a node is. */
enum class NodeKind : uint8_t
{
  System,
  Service,
  GameObject,
  Component,
  SceneData,
  Event,
  ScriptableObject,
  MeshSource,
  Utils,
};

// The node kinds as the JSON names them, in NodeKind's order.
constexpr std::array<std::string_view, 9> NODE_KINDS = {"system",
                                                        "service",
                                                        "gameObject",
                                                        "component",
                                                        "sceneData",
                                                        "event",
                                                        "scriptableObject",
                                                        "meshSource",
                                                        "utils"};

/** How a term list's components are taken. */
enum class TermUse : uint8_t
{
  Rows,      // query rows: bound ones read or written, the rest filter
  Registers, // registerComponent()
  Observes,  // observe()
  Filters,   // matches(): nothing bound
};

/** One node of the graph. */
struct Node
{
  NodeKind kind = NodeKind::Component;
  std::string name;
  std::string qualifiedName;
  std::filesystem::path file;
  unsigned line = 0;
  bool declared = false;
  bool marked = false;
  // An event's role ("news", "entityNews", "request", "response"); empty
  // when it isn't known.
  std::string role;
  // A request's response types, by node id.
  std::string success;
  std::string progress;
  std::string failure;
  // A utils node's files.
  std::vector<std::filesystem::path> files;
};

/** One place an edge is written. */
struct Site
{
  uint32_t file = 0; // into GraphBuilder::m_siteFiles
  unsigned line = 0;
  std::string_view via;
  std::string_view term;
};

/** A type spelling without its qualifiers. */
struct BareType
{
  std::string name;
  bool isConst = false;
};

/** An #include of a file whose node is known once every file is read. */
struct Include
{
  std::filesystem::path file;
  std::string actor; // the file's one system, service or game object
  std::string spelled;
  unsigned line = 0;
};

/** An exported file, parsed once and kept until its edges are read. */
struct ParsedFile
{
  explicit ParsedFile(const std::filesystem::path& path) : path(path) {}

  std::filesystem::path path;
  std::string source;
  std::unique_ptr<TSTree, decltype(&ts_tree_delete)> tree{nullptr,
                                                          &ts_tree_delete};
};

/** One exported file's walk: what it calls, brings in and includes. */
struct FileWalk : lint::LintFile
{
  std::vector<TSNode> calls;
  std::vector<TSNode> includes;
  std::vector<std::string_view> classes; // classes defined or written for
  std::vector<UsingName> usings;
  std::vector<ResponseSend> sends;
  std::vector<std::string> sendFrom;     // the node each send is written in
  std::vector<std::string_view> sendVia; // and the method it's sent with
  std::vector<uint32_t> lineStarts;
  std::vector<lint::TermMember> members;
  std::string_view actor; // the file's one system, service or game object
};

// The SFS API calls the export reads, which are a site's "via".
constexpr std::array<std::string_view, 29> API_NAMES = {"addComponent",
                                                        "addSceneData",
                                                        "addTag",
                                                        "broadcast",
                                                        "childrenOf",
                                                        "consumer",
                                                        "descendants",
                                                        "fail",
                                                        "find",
                                                        "finder",
                                                        "getComponent",
                                                        "getEntities",
                                                        "getSystem",
                                                        "hasComponent",
                                                        "hasSystem",
                                                        "matches",
                                                        "observe",
                                                        "progress",
                                                        "query",
                                                        "registerComponent",
                                                        "removeComponent",
                                                        "removeSceneData",
                                                        "request",
                                                        "requester",
                                                        "respond",
                                                        "sceneData",
                                                        "send",
                                                        "tryGetComponent",
                                                        "tryGetSystem"};

// `name` as API_NAMES holds it, which outlives the file it's read from;
// empty for a name that isn't one of them.
std::string_view apiName(std::string_view name)
{
  for (const std::string_view known : API_NAMES)
    if (known == name)
      return known;

  return {};
}

// A type as written without what doesn't name it: `const`, `volatile`,
// `typename`, a leading `::`, and the pointer or reference it's taken by.
BareType bareType(std::string_view spelled)
{
  BareType bare;
  std::string type = lint::tidy(spelled);

  for (bool changed = true; changed;)
  {
    changed = false;

    for (const std::string_view prefix :
         {"const ", "volatile ", "typename ", "struct ", "class ", "::"})
      if (type.starts_with(prefix))
      {
        bare.isConst = bare.isConst || prefix == "const ";
        type.erase(0, prefix.size());
        changed = true;
      }

    for (const std::string_view suffix : {" const", " volatile", "&", "*", " "})
      if (type.ends_with(suffix))
      {
        bare.isConst = bare.isConst || suffix == " const";
        type.erase(type.size() - suffix.size());
        changed = true;
      }
  }

  bare.name = codegen::normaliseType(type);
  return bare;
}

// Whether `name` is a template parameter of a template or a generic lambda
// around `at`.
bool isTemplateParameter(const FileWalk& walk, TSNode at, std::string_view name)
{
  for (TSNode up = lint::parent(at); !ts_node_is_null(up);
       up = lint::parent(up))
  {
    TSNode parameters;

    if (walk.kind(up) == Kind::TemplateDeclaration)
      parameters = walk.field(up, "parameters");
    else if (walk.kind(up) == Kind::LambdaExpression)
      parameters = walk.field(up, "template_parameters");
    else
      continue;

    for (uint32_t i = 0; i < namedCount(parameters); ++i)
    {
      const TSNode parameter = namedChild(parameters, i);

      for (uint32_t c = 0; c < namedCount(parameter); ++c)
        if (walk.text(namedChild(parameter, c)) == name)
          return true;
    }
  }

  return false;
}

/** Builds one export. */
class GraphBuilder
{
public:
  GraphBuilder(const std::filesystem::path& root,
               std::span<const std::filesystem::path> files,
               const GraphProgress& progress)
      : m_root(root), m_files(files), m_progress(progress)
  {
  }

  Json run();

private:
  std::string actorId(std::string_view name);
  std::optional<NodeKind> actorKind(std::string_view name) const;
  void addEdge(const std::string& from,
               const std::string& to,
               EdgeKind kind,
               const std::filesystem::path& file,
               unsigned line,
               std::string_view via,
               std::string_view term = {});
  void addTermEdges(FileWalk& walk,
                    TSNode call,
                    TSNode arguments,
                    TermUse use,
                    std::string_view via,
                    bool knownOnly = false);
  void addTypeEdge(FileWalk& walk,
                   TSNode call,
                   std::string_view spelled,
                   NodeKind use,
                   EdgeKind kind,
                   std::string_view via);
  void addAccessEdge(FileWalk& walk,
                     TSNode call,
                     std::string_view spelled,
                     NodeKind use,
                     std::string_view via);
  void collect(FileWalk& walk, TSNode root);
  std::string declaredNode(const IndexEntry& entry);
  std::string eventNode(const codegen::EventModel& event);
  std::string externalNode(NodeKind kind,
                           std::string qualifiedName,
                           std::string_view role = {});
  void exportFile(const ParsedFile& parsed);
  std::string fromId(FileWalk& walk, TSNode at);
  bool isExported(const std::filesystem::path& file) const;
  unsigned
  lineOf(const FileWalk& walk, std::string_view spelled, TSNode fallback) const;
  void parseAll(std::vector<ParsedFile>& files);
  void readCall(FileWalk& walk, TSNode call);
  void readSends(FileWalk& walk);
  std::string relative(const std::filesystem::path& file) const;
  std::string requestResponse(const codegen::EventModel& request,
                              const std::string& listed);
  void resolveIncludes();
  void step(std::string_view phase);
  Json toJson();
  std::string typeNode(FileWalk& walk,
                       TSNode at,
                       std::string_view spelled,
                       NodeKind use,
                       bool* isConst = nullptr);
  std::string utilsId(const std::filesystem::path& file);

  std::filesystem::path m_root;
  std::span<const std::filesystem::path> m_files; // sorted
  const GraphProgress& m_progress;
  std::size_t m_done = 0;
  std::size_t m_total = 0;
  ProjectIndex m_index;
  Linter m_linter;
  FileWalk m_walk;
  std::vector<codegen::Diagnostic> m_unused; // the walk's report() target
  std::map<std::string, Node> m_nodes;       // by id
  std::map<std::tuple<std::string, std::string, EdgeKind>, std::vector<Site>>
      m_edges;
  std::vector<std::string> m_siteFiles; // relative paths, by Site::file
  std::map<std::filesystem::path, uint32_t> m_siteFileIndex;
  std::vector<Include> m_includes;
};

std::string GraphBuilder::actorId(std::string_view name)
{
  const IndexEntry* entry = m_index.find(name);
  return entry ? declaredNode(*entry) : std::string{};
}

std::optional<NodeKind> GraphBuilder::actorKind(std::string_view name) const
{
  if (name.empty())
    return std::nullopt;

  switch (m_index.rootKind(name))
  {
  case codegen::TypeKind::System:
    return NodeKind::System;
  case codegen::TypeKind::Service:
    return NodeKind::Service;
  case codegen::TypeKind::GameObject:
    return NodeKind::GameObject;
  default:
    return std::nullopt;
  }
}

void GraphBuilder::addAccessEdge(FileWalk& walk,
                                 TSNode call,
                                 std::string_view spelled,
                                 NodeKind use,
                                 std::string_view via)
{
  bool isConst = false;
  const std::string to = typeNode(walk, call, spelled, use, &isConst);

  if (to.empty())
    return;

  addEdge(fromId(walk, call),
          to,
          isConst ? EdgeKind::Reads : EdgeKind::Writes,
          *walk.path,
          lineOf(walk, spelled, call),
          via);
}

void GraphBuilder::addEdge(const std::string& from,
                           const std::string& to,
                           EdgeKind kind,
                           const std::filesystem::path& file,
                           unsigned line,
                           std::string_view via,
                           std::string_view term)
{
  if (from.empty() || to.empty())
    return;

  const auto [at, added] = m_siteFileIndex.try_emplace(
      file, static_cast<uint32_t>(m_siteFiles.size()));

  if (added)
    m_siteFiles.push_back(relative(file));

  m_edges[{from, to, kind}].push_back({at->second, line, via, term});
}

void GraphBuilder::addTermEdges(FileWalk& walk,
                                TSNode call,
                                TSNode arguments,
                                TermUse use,
                                std::string_view via,
                                bool knownOnly)
{
  const std::string_view owner = walk.ownerAt(call);
  walk.members.clear();

  // A term that can't be read (a pack) still gives the members before it.
  // A query alias named through its class or namespace (`Parts::Rows`)
  // stands for its terms too.
  for (uint32_t i = 0; i < namedCount(arguments); ++i)
  {
    const std::string_view term =
        lint::trimmed(walk.text(namedChild(arguments, i)));
    const std::size_t colons = term.rfind("::");
    std::string_view inner;
    const QueryAlias* named =
        colons == std::string_view::npos ||
                walk.wrapperOfText(call, term, inner) != Wrapper::None
            ? nullptr
            : walk.alias(lastName(term.substr(0, colons)), lastName(term));
    bool read = true;

    if (named)
      for (const std::string& nested : named->terms)
        read = read && walk.termMembers(call, owner, nested, walk.members, 1);
    else
      read = walk.termMembers(call, owner, term, walk.members);

    if (!read)
      break;
  }

  const std::string from = fromId(walk, call);

  for (const lint::TermMember& member : walk.members)
  {
    // Another library's query<...>() names no components: one on something
    // that isn't known to be a registry (a template's store) takes only the
    // components the files declare and SFS's.
    if (knownOnly)
    {
      const BareType bare = bareType(member.spelling);

      if (!m_index.find(lastName(bare.name)) && !bare.name.starts_with("sfs::"))
        continue;
    }

    bool isConst = false;
    const std::string to =
        typeNode(walk, call, member.spelling, NodeKind::Component, &isConst);

    if (to.empty())
      continue;

    std::string_view term = "required";

    if (member.group == Wrapper::Any)
      term = "any";
    else if (member.group == Wrapper::With)
      term = "with";
    else if (member.group == Wrapper::Excluding)
      term = "excluding";

    EdgeKind kind = EdgeKind::Filters;

    if (use == TermUse::Registers)
      kind = EdgeKind::Registers;
    else if (use == TermUse::Observes)
      kind = EdgeKind::Observes;
    else if (use == TermUse::Rows &&
             (member.group == Wrapper::None || member.group == Wrapper::Any))
      kind = isConst ? EdgeKind::Reads : EdgeKind::Writes;

    addEdge(from,
            to,
            kind,
            *walk.path,
            lineOf(walk, member.spelling, call),
            via,
            term);
  }
}

void GraphBuilder::addTypeEdge(FileWalk& walk,
                               TSNode call,
                               std::string_view spelled,
                               NodeKind use,
                               EdgeKind kind,
                               std::string_view via)
{
  std::string to = typeNode(walk, call, spelled, use);

  // An event the files don't declare is a request when it's asked for.
  if (!to.empty() && kind == EdgeKind::Requests)
    if (Node& node = m_nodes[to]; node.role.empty())
      node.role = "request";

  addEdge(fromId(walk, call),
          to,
          kind,
          *walk.path,
          lineOf(walk, spelled, call),
          via);
}

void GraphBuilder::collect(FileWalk& walk, TSNode root)
{
  // ERROR subtrees are walked too: a call in code tree-sitter can't place
  // (a macro around it) is still a call.
  lint::walkTree(
      root,
      [&](TSNode node)
      {
        switch (walk.kind(node))
        {
        case Kind::CallExpression:
          walk.calls.push_back(node);
          break;
        case Kind::UsingDeclaration:
          walk.readUsing(node);
          noteUsing(node, walk.source, *walk.path, walk.usings);
          break;
        case Kind::AliasDeclaration:
        case Kind::TypeDefinition:
        case Kind::NamespaceAliasDefinition:
          noteUsing(node, walk.source, *walk.path, walk.usings);
          break;
        case Kind::PreprocInclude:
          walk.includes.push_back(node);
          break;
        case Kind::ClassSpecifier:
          if (!ts_node_is_null(walk.field(node, "body")))
            walk.classes.push_back(walk.text(walk.field(node, "name")));

          break;
        case Kind::FunctionDefinition:
          walk.classes.push_back(lint::definedClassOf(node, walk.source));
          break;
        default:
          break;
        }

        return lint::Next::Descend;
      });
}

std::string GraphBuilder::declaredNode(const IndexEntry& entry)
{
  NodeKind kind = NodeKind::ScriptableObject;

  switch (m_index.rootKind(entry.name))
  {
  case codegen::TypeKind::Component:
    kind = NodeKind::Component;
    break;
  case codegen::TypeKind::System:
    kind = NodeKind::System;
    break;
  case codegen::TypeKind::Service:
    kind = NodeKind::Service;
    break;
  case codegen::TypeKind::GameObject:
    kind = NodeKind::GameObject;
    break;
  case codegen::TypeKind::MeshSource:
    kind = NodeKind::MeshSource;
    break;
  case codegen::TypeKind::SceneData:
    kind = NodeKind::SceneData;
    break;
  case codegen::TypeKind::ScriptableObject:
  case codegen::TypeKind::Derived:
    break;
  }

  std::string id = std::string(NODE_KINDS[static_cast<std::size_t>(kind)]) +
                   ":" + entry.qualifiedName;
  const auto [at, added] = m_nodes.try_emplace(id);

  if (added)
  {
    Node& node = at->second;
    node.kind = kind;
    node.name = entry.name;
    node.qualifiedName = entry.qualifiedName;
    node.file = entry.header;
    node.line = entry.line;
    node.declared = true;
    node.marked = entry.marked;
  }

  return id;
}

std::string GraphBuilder::eventNode(const codegen::EventModel& event)
{
  std::string id = "event:" + event.qualifiedName;
  Node& node = m_nodes[id];

  if (node.declared)
    return id;

  node.kind = NodeKind::Event;
  node.name = event.name;
  node.qualifiedName = event.qualifiedName;
  node.file = event.header;
  node.line = event.nameRange.startRow + 1;
  node.declared = true;

  switch (event.kind)
  {
  case codegen::EventKind::News:
    node.role = "news";
    break;
  case codegen::EventKind::EntityNews:
    node.role = "entityNews";
    break;
  case codegen::EventKind::Request:
    node.role = "request";
    break;
  case codegen::EventKind::Response:
    node.role = "response";
    break;
  }

  if (event.kind != codegen::EventKind::Request)
    return id;

  node.success = requestResponse(event, event.success);
  node.progress = requestResponse(event, event.progress);
  node.failure = requestResponse(event, event.failure);
  return id;
}

void GraphBuilder::exportFile(const ParsedFile& parsed)
{
  const std::filesystem::path& file = parsed.path;
  const std::string& source = parsed.source;
  const TSTree* tree = parsed.tree.get();
  FileWalk& walk = m_walk;
  walk.resetFile();
  walk.calls.clear();
  walk.includes.clear();
  walk.classes.clear();
  walk.usings.clear();
  walk.sends.clear();
  walk.sendFrom.clear();
  walk.sendVia.clear();
  walk.linter = &m_linter;
  walk.source = source;
  walk.path = &file;
  walk.index = &m_index;
  walk.out = &m_unused;
  walk.lineStarts = countLines(source);
  findQueryAliases(tree, source, file, walk.aliases);
  collect(walk, ts_tree_root_node(tree));

  // The file's one system, service or game object, which its free functions
  // and helper classes work for.
  walk.actor = {};
  bool several = false;

  for (const std::string_view name : walk.classes)
  {
    if (!actorKind(name) || name == walk.actor)
      continue;

    several = several || !walk.actor.empty();
    walk.actor = name;
  }

  if (several)
    walk.actor = {};

  for (const TSNode call : walk.calls)
    readCall(walk, call);

  readSends(walk);

  for (const TSNode include : walk.includes)
  {
    std::string_view spelled =
        lint::trimmed(walk.text(walk.field(include, "path")));

    if (spelled.size() < 2)
      continue;

    spelled = spelled.substr(1, spelled.size() - 2);
    m_includes.push_back({file,
                          std::string(walk.actor),
                          std::string(spelled),
                          ts_node_start_point(include).row + 1});
  }
}

std::string GraphBuilder::externalNode(NodeKind kind,
                                       std::string qualifiedName,
                                       std::string_view role)
{
  std::string id = std::string(NODE_KINDS[static_cast<std::size_t>(kind)]) +
                   ":" + qualifiedName;
  const auto [at, added] = m_nodes.try_emplace(id);

  if (added)
  {
    Node& node = at->second;
    node.kind = kind;
    node.name = std::string(lastName(qualifiedName));
    node.qualifiedName = std::move(qualifiedName);
    node.role = std::string(role);
  }

  return id;
}

std::string GraphBuilder::fromId(FileWalk& walk, TSNode at)
{
  for (TSNode up = lint::parent(at); !ts_node_is_null(up);
       up = lint::parent(up))
  {
    std::string_view name;

    if (walk.kind(up) == Kind::ClassSpecifier)
      name = walk.text(walk.field(up, "name"));
    else if (walk.kind(up) == Kind::FunctionDefinition)
      name = lint::definedClassOf(up, walk.source);

    if (actorKind(name))
      return actorId(name);
  }

  return walk.actor.empty() ? utilsId(*walk.path) : actorId(walk.actor);
}

bool GraphBuilder::isExported(const std::filesystem::path& file) const
{
  return std::binary_search(
      m_files.begin(), m_files.end(), file.lexically_normal());
}

unsigned GraphBuilder::lineOf(const FileWalk& walk,
                              std::string_view spelled,
                              TSNode fallback) const
{
  const char* begin = walk.source.data();

  if (spelled.data() < begin || spelled.data() >= begin + walk.source.size())
    return ts_node_start_point(fallback).row + 1;

  const auto offset = static_cast<uint32_t>(spelled.data() - begin);
  const auto after =
      std::upper_bound(walk.lineStarts.begin(), walk.lineStarts.end(), offset);
  return static_cast<unsigned>(after - walk.lineStarts.begin());
}

void GraphBuilder::parseAll(std::vector<ParsedFile>& files)
{
  // Files are parsed on a few threads, each with a parser of its own, taking
  // the next file as they finish one: parsing is most of an export's time.
  std::atomic<std::size_t> next = 0;
  std::mutex reporting;
  const auto work = [&]
  {
    const ParserPtr parser = makeCppParser();

    for (std::size_t i = next++; i < files.size(); i = next++)
    {
      ParsedFile& file = files[i];
      file.source = codegen::readFile(file.path);
      file.tree.reset(
          ts_parser_parse_string(parser.get(),
                                 nullptr,
                                 file.source.data(),
                                 static_cast<uint32_t>(file.source.size())));
      const std::lock_guard lock(reporting);
      ++m_done;
      step("parse");
    }
  };
  const unsigned threads =
      std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
  std::vector<std::jthread> helpers;
  helpers.reserve(threads - 1);

  for (unsigned i = 1; i < threads; ++i)
    helpers.emplace_back(work);

  work();
}

void GraphBuilder::readCall(FileWalk& walk, TSNode call)
{
  TSNode arguments;
  std::string_view name = walk.calleeName(call, arguments);

  // A qualified call: `sfs::EventVerbs::consumer<E>()`, or a typed
  // hierarchy walk, `sfs::hierarchy::childrenOf<Terms>(e)`.
  if (name.empty())
  {
    TSNode function = walk.field(call, "function");

    while (walk.kind(function) == Kind::QualifiedIdentifier)
      function = walk.field(function, "name");

    if (walk.kind(function) != Kind::TemplateFunction)
      return;

    name = walk.text(walk.field(function, "name"));
    arguments = walk.field(function, "arguments");

    if (name != "consumer" && name != "requester" && name != "childrenOf" &&
        name != "descendants")
      return;
  }

  const uint32_t count = namedCount(arguments);
  const std::string_view first = walk.text(namedChild(arguments, 0));
  name = apiName(name);

  if (name == "respond" || name == "progress" || name == "fail")
  {
    const std::size_t before = walk.sends.size();
    noteResponseSend(call, walk.source, *walk.path, walk.sends);

    if (walk.sends.size() > before)
    {
      walk.sendFrom.push_back(fromId(walk, call));
      walk.sendVia.push_back(name);
    }

    return;
  }

  if (name == "send")
  {
    const std::string_view spelled =
        count > 0 ? first : sentType(call, walk.source);
    const BareType bare = bareType(spelled);
    const TSNode receiver = walk.receiverOf(call);
    std::string events(walk.text(receiver));

    for (char& c : events)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Another library's send() sends no event: a send is taken when it's a
    // system's, service's or game object's own, an Events' or a broadcast's,
    // or its type is an event.
    const bool own =
        (ts_node_is_null(receiver) || walk.kind(receiver) == Kind::This) &&
        !fromId(walk, call).starts_with("utils:");
    const bool eventsSend = own || events.ends_with("events") ||
                            events.ends_with("events()") ||
                            events.find("broadcast") != std::string::npos;

    if (!bare.name.empty() &&
        (eventsSend || m_index.findEvent(lastName(bare.name)) ||
         bare.name.starts_with("sfs::")))
      addTypeEdge(walk, call, spelled, NodeKind::Event, EdgeKind::Sends, name);

    return;
  }

  if (name == "request" && !ts_node_is_null(walk.receiverOf(call)))
  {
    const std::string_view spelled = sentType(call, walk.source);
    const codegen::EventModel* event =
        spelled.empty() ? nullptr : m_index.findEvent(lastName(spelled));

    if ((event && event->kind == codegen::EventKind::Request) ||
        lint::trimmed(spelled).starts_with("sfs::"))
      addTypeEdge(
          walk, call, spelled, NodeKind::Event, EdgeKind::Requests, name);

    return;
  }

  if (count == 0)
    return;

  if (name == "getComponent" || name == "tryGetComponent" || name == "finder" ||
      (name == "find" && walk.isRegistryCall(call)))
    addAccessEdge(walk, call, first, NodeKind::Component, name);
  else if (name == "sceneData")
    addAccessEdge(walk, call, first, NodeKind::SceneData, name);
  else if (name == "hasComponent")
    addTypeEdge(
        walk, call, first, NodeKind::Component, EdgeKind::Filters, name);
  else if (name == "addComponent" || name == "addTag")
    addTypeEdge(walk, call, first, NodeKind::Component, EdgeKind::Adds, name);
  else if (name == "removeComponent")
    addTypeEdge(
        walk, call, first, NodeKind::Component, EdgeKind::Removes, name);
  else if (name == "addSceneData")
    addTypeEdge(walk, call, first, NodeKind::SceneData, EdgeKind::Adds, name);
  else if (name == "removeSceneData")
    addTypeEdge(
        walk, call, first, NodeKind::SceneData, EdgeKind::Removes, name);
  else if (name == "consumer")
    addTypeEdge(walk, call, first, NodeKind::Event, EdgeKind::Consumes, name);
  else if (name == "requester")
    addTypeEdge(walk, call, first, NodeKind::Event, EdgeKind::Requests, name);
  else if (name == "broadcast")
    addTypeEdge(walk, call, first, NodeKind::Event, EdgeKind::Sends, name);
  else if (name == "registerComponent")
    addTermEdges(walk, call, arguments, TermUse::Registers, name);
  else if (name == "observe")
    addTermEdges(walk, call, arguments, TermUse::Observes, name);
  else if (name == "matches")
    addTermEdges(walk, call, arguments, TermUse::Filters, name);
  else if (name == "query" || name == "getEntities" || name == "childrenOf" ||
           name == "descendants")
    addTermEdges(walk,
                 call,
                 arguments,
                 TermUse::Rows,
                 name,
                 name == "query" && !walk.isRegistryCall(call));
  else if (name == "getSystem" || name == "tryGetSystem" || name == "hasSystem")
  {
    const std::string from = fromId(walk, call);
    const std::string to = typeNode(walk, call, first, NodeKind::System);

    if (to != from)
      addEdge(from,
              to,
              EdgeKind::ReadsSystem,
              *walk.path,
              lineOf(walk, first, call),
              name);
  }
}

void GraphBuilder::readSends(FileWalk& walk)
{
  const auto kindOf = [&](std::string_view name)
  { return m_index.responseKind(name); };

  for (std::size_t i = 0; i < walk.sends.size(); ++i)
  {
    const ResponseSend& send = walk.sends[i];
    const std::string type = resolveResponse(send, walk.usings, kindOf);
    std::string to;

    if (const codegen::EventModel* event =
            type.empty() ? nullptr : m_index.findEvent(type);
        event && event->qualifiedName == type)
      to = eventNode(*event);
    else if (!type.empty())
      to = externalNode(NodeKind::Event, type, "response");
    else if (send.spelling.starts_with("sfs::"))
      to = externalNode(NodeKind::Event, send.spelling, "response");

    addEdge(walk.sendFrom[i],
            to,
            EdgeKind::Responds,
            send.file,
            send.range.startRow + 1,
            walk.sendVia[i]);
  }
}

std::string GraphBuilder::relative(const std::filesystem::path& file) const
{
  const std::filesystem::path within = file.lexically_relative(m_root);

  if (within.empty() || *within.begin() == "..")
    return file.generic_string();

  return within.generic_string();
}

std::string GraphBuilder::requestResponse(const codegen::EventModel& request,
                                          const std::string& listed)
{
  if (listed.empty())
    return {};

  const codegen::NameLookup lookup = codegen::lookUpName(
      listed, request.scopes, request.offset, request.usings);

  for (const std::string& candidate : lookup.candidates)
    if (const codegen::EventModel* event = m_index.findEvent(candidate);
        event && event->qualifiedName == candidate)
      return eventNode(*event);

  std::string name = codegen::normaliseType(listed);

  if (!name.starts_with("sfs::") && request.qualifiedName.starts_with("sfs::"))
    name = "sfs::" + name;

  return externalNode(NodeKind::Event, std::move(name), "response");
}

void GraphBuilder::resolveIncludes()
{
  // Each utils node by its files' paths without extensions, so a header and
  // its source are one.
  std::map<std::filesystem::path, std::string> utils;

  for (const auto& [id, node] : m_nodes)
    if (node.kind == NodeKind::Utils)
      for (const std::filesystem::path& file : node.files)
        utils.emplace(std::filesystem::path(file).replace_extension(), id);

  for (const Include& include : m_includes)
  {
    std::string from =
        include.actor.empty() ? std::string{} : actorId(include.actor);

    if (from.empty())
    {
      const auto own =
          utils.find(std::filesystem::path(include.file).replace_extension());

      if (own == utils.end())
        continue;

      from = own->second;
    }

    // The include is looked for beside the file, then in each folder above
    // it up to the root, and in each one's include folder.
    std::string to;

    for (std::filesystem::path folder = include.file.parent_path();
         to.empty() && !folder.empty();
         folder = folder.parent_path())
    {
      for (const std::filesystem::path& candidate :
           {folder / include.spelled, folder / "include" / include.spelled})
        if (const auto found =
                utils.find(candidate.lexically_normal().replace_extension());
            found != utils.end())
        {
          to = found->second;
          break;
        }

      if (folder == m_root || folder == folder.parent_path())
        break;
    }

    if (!to.empty() && to != from)
      addEdge(
          from, to, EdgeKind::UsesUtil, include.file, include.line, "include");
  }
}

Json GraphBuilder::run()
{
  const ParserPtr parser = makeCppParser();
  m_index.start(m_root);
  m_index.addFiles(m_files);
  m_total = m_files.size() + m_index.pendingCount() + m_files.size() + 1;
  step("scan");

  // Each exported file is parsed once, its tree indexed and kept for its
  // edges, which need every file indexed first.
  std::vector<ParsedFile> parsed(m_files.begin(), m_files.end());
  parseAll(parsed);

  for (const ParsedFile& file : parsed)
  {
    if (file.tree)
      m_index.indexFile(file.path, file.tree.get(), file.source);

    ++m_done;
    step("index");
  }

  while (m_index.pending())
  {
    m_index.indexSlice(parser.get(), 1);
    ++m_done;
    step("index");
  }

  for (const ParsedFile& file : parsed)
  {
    if (file.tree)
      exportFile(file);

    ++m_done;
    step("edges");
  }

  // Every type the exported files declare is a node, touched or not.
  for (const ScriptFile* file : m_index.files())
  {
    for (const IndexEntry& entry : file->entries)
      if (isExported(entry.header))
        declaredNode(entry);

    for (const codegen::EventModel& event : file->events)
      if (isExported(event.header))
        eventNode(event);
  }

  resolveIncludes();
  Json json = toJson();
  m_done = m_total;
  step("write");
  return json;
}

void GraphBuilder::step(std::string_view phase)
{
  if (m_progress)
    m_progress(m_done, m_total, phase);
}

Json GraphBuilder::toJson()
{
  Json nodes = Json::array();

  for (auto& [id, node] : m_nodes)
  {
    Json item;
    item["id"] = id;
    item["kind"] = NODE_KINDS[static_cast<std::size_t>(node.kind)];
    item["name"] = node.name;
    item["qualifiedName"] = node.qualifiedName;

    if (node.kind == NodeKind::Utils && !node.files.empty())
    {
      item["file"] = relative(node.files.front());
      item["line"] = 1;
    }
    else if (node.declared && node.kind != NodeKind::Utils)
    {
      item["file"] = relative(node.file);
      item["line"] = node.line;
    }

    item["declared"] = node.declared;
    item["engine"] = node.qualifiedName.starts_with("sfs::");

    if (node.kind == NodeKind::Event)
    {
      item["role"] = node.role.empty() ? Json() : Json(node.role);

      if (!node.success.empty())
      {
        Json responses;
        responses["success"] = node.success;

        if (!node.progress.empty())
          responses["progress"] = node.progress;

        if (!node.failure.empty())
          responses["failure"] = node.failure;

        item["responses"] = std::move(responses);
      }
    }

    if (node.declared &&
        (node.kind == NodeKind::Component || node.kind == NodeKind::SceneData))
      item["marked"] = node.marked;

    if (node.kind == NodeKind::Utils)
    {
      Json files = Json::array();

      for (const std::filesystem::path& file : node.files)
        files.push_back(relative(file));

      item["files"] = std::move(files);
    }

    nodes.push_back(std::move(item));
  }

  Json edges = Json::array();

  for (auto& [key, sites] : m_edges)
  {
    const auto order = [&](const Site& a, const Site& b)
    {
      return std::tie(m_siteFiles[a.file], a.line, a.via, a.term) <
             std::tie(m_siteFiles[b.file], b.line, b.via, b.term);
    };
    const auto same = [](const Site& a, const Site& b)
    {
      return a.file == b.file && a.line == b.line && a.via == b.via &&
             a.term == b.term;
    };
    std::sort(sites.begin(), sites.end(), order);
    sites.erase(std::unique(sites.begin(), sites.end(), same), sites.end());

    Json edge;
    edge["from"] = std::get<0>(key);
    edge["to"] = std::get<1>(key);
    edge["kind"] = EDGE_KINDS[static_cast<std::size_t>(std::get<2>(key))];
    Json list = Json::array();

    for (const Site& site : sites)
    {
      Json item;
      item["file"] = m_siteFiles[site.file];
      item["line"] = site.line;
      item["via"] = site.via;

      if (!site.term.empty())
        item["term"] = site.term;

      list.push_back(std::move(item));
    }

    edge["sites"] = std::move(list);
    edges.push_back(std::move(edge));
  }

  Json json;
  json["version"] = GRAPH_VERSION;
  json["root"] = m_root.generic_string();
  json["nodes"] = std::move(nodes);
  json["edges"] = std::move(edges);
  return json;
}

std::string GraphBuilder::typeNode(FileWalk& walk,
                                   TSNode at,
                                   std::string_view spelled,
                                   NodeKind use,
                                   bool* isConst)
{
  const BareType bare = bareType(spelled);

  if (isConst)
    *isConst = bare.isConst;

  if (bare.name.empty())
    return {};

  const std::string_view last = lastName(bare.name);

  // Generic code's template parameters, and the types named through them
  // (`typename TList::Item`), name no type of their own; nor does a call or
  // std's `std::forward<T>`.
  const std::string_view head =
      std::string_view(bare.name).substr(0, bare.name.find("::"));

  if (bare.name.find('(') != std::string::npos || head == "std" ||
      isTemplateParameter(walk, at, head))
    return {};

  if (use == NodeKind::Event)
  {
    const codegen::EventModel* event = m_index.findEvent(bare.name);

    if (!event)
      event = m_index.findEvent(last);

    if (event)
      return eventNode(*event);
  }
  else if (const IndexEntry* entry = m_index.find(last))
    return declaredNode(*entry);

  // A type the files don't declare: the engine's when it's written as SFS's,
  // or its first name is looked up in SFS where it's written.
  std::string name = bare.name;

  if (!name.starts_with("sfs::") && walk.isSfsNameAt(at, head, head))
    name = "sfs::" + name;

  return externalNode(use, std::move(name));
}

std::string GraphBuilder::utilsId(const std::filesystem::path& file)
{
  std::filesystem::path stem = file;
  stem.replace_extension();
  std::string id = "utils:" + relative(stem);
  const auto [at, added] = m_nodes.try_emplace(id);

  if (!added)
    return id;

  // A utils node is every exported file of its name: a header and its
  // source.
  Node& node = at->second;
  node.kind = NodeKind::Utils;
  node.name = stem.filename().string();
  node.qualifiedName = relative(stem);
  node.declared = true;

  for (const std::filesystem::path& exported : m_files)
    if (exported.parent_path() == file.parent_path() &&
        exported.stem() == stem.filename())
      node.files.push_back(exported);

  return id;
}

} // namespace

nlohmann::ordered_json exportGraph(const std::filesystem::path& root,
                                   std::span<const std::filesystem::path> files,
                                   const GraphProgress& progress)
{
  return GraphBuilder(root, files, progress).run();
}

int runGraph(const std::vector<std::string>& args)
{
  bool progress = false;
  std::vector<std::string> rest;

  for (const std::string& arg : args)
    if (arg == "--progress")
      progress = true;
    else
      rest.push_back(arg);

  CommandRun run;

  if (!readCommandRun(rest, false, run))
    return 2;

  GraphProgress report;

  if (progress)
    report = [](std::size_t done, std::size_t total, std::string_view phase)
    {
      std::cerr << "progress " << done << ' ' << total << ' ' << phase
                << std::endl;
    };

  const Json json = exportGraph(run.root, run.files, report);
  std::cout << json.dump(1) << '\n' << std::flush;
  return 0;
}

} // namespace sfs::lsp
