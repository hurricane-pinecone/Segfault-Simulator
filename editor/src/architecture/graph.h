#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A game's architecture as sfs-lsp's `--graph` export has it (see
// docs/editor/sfs-lsp, "Architecture export"): its systems, components, events
// and the rest as nodes, and what each one's code does with the others as
// edges. Read once per export and never changed: the Architecture board draws
// it.
namespace architecture
{

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

/** How many node kinds there are. */
inline constexpr std::size_t NODE_KINDS = 9;

/** What an edge's code does. */
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

/** How many edge kinds there are. */
inline constexpr std::size_t EDGE_KINDS = 13;

/** No file: a node that isn't declared in the files read. */
inline constexpr uint32_t NO_FILE = UINT32_MAX;

/** A place in the code an edge is written. */
struct Site
{
  /** Its file: an index into Graph::files. */
  uint32_t file = 0;
  /** Its line, from 1. */
  int line = 0;
  /** The call it's made with (`getComponent`, `query`, `send`, ...). */
  std::string via;
  /** A query or registration term's role (`required`, `with`, ...), or "". */
  std::string term;
};

/** A type (or a utils file) of the game's, or of the engine's it uses. */
struct Node
{
  /** `kind:qualifiedName`, stable from run to run. */
  std::string id;
  /** Its name, as the board titles it. */
  std::string name;
  /** Its name with its namespaces. */
  std::string qualifiedName;
  /** An event's role (`news`, `request`, ...); "" if none or unknown. */
  std::string role;
  /** What it is. */
  NodeKind kind = NodeKind::Component;
  /** Its declaration's file (an index into Graph::files); NO_FILE if none. */
  uint32_t file = NO_FILE;
  /** Its declaration's line, from 1; 0 if it isn't declared in the files. */
  int line = 0;
  /**
   * Every file it's made of (indices into Graph::files): a utils node's
   * header and source; empty for a type, which has `file`.
   */
  std::vector<uint32_t> files;
  /** Whether the files read declare it. */
  bool declared = false;
  /** Whether it's the engine's own. */
  bool engine = false;
};

/** What one node's code does with another, and everywhere it's written. */
struct Edge
{
  /** The node whose code it is: an index into Graph::nodes. */
  uint32_t from = 0;
  /** The node it's done to: an index into Graph::nodes. */
  uint32_t to = 0;
  /** What it does. */
  EdgeKind kind = EdgeKind::Reads;
  /** Where it's written, by file and line. */
  std::vector<Site> sites;
};

/** A whole export, its edges indexed by the nodes they touch. */
struct Graph
{
  /** The edges touching a node (from or to it), as indices into `edges`. */
  std::span<const uint32_t> edgesOf(uint32_t node) const
  {
    return std::span<const uint32_t>(touching).subspan(
        touchingStart[node], touchingStart[node + 1] - touchingStart[node]);
  }

  /** The node with this id; nullopt if there's none. */
  std::optional<uint32_t> find(std::string_view id) const;

  /** The project root the export's paths are relative to (sfs-lsp's view). */
  std::string root;
  /** Every file named, relative to the root: nodes and sites index it. */
  std::vector<std::string> files;
  /** The nodes, sorted by id. */
  std::vector<Node> nodes;
  /** The edges, sorted by from, to and kind. */
  std::vector<Edge> edges;
  /** Every node's touching edges, one node after another (see edgesOf()). */
  std::vector<uint32_t> touching;
  /** Where each node's run in `touching` starts; one more for the end. */
  std::vector<uint32_t> touchingStart;
};

/** How far an export has got, from one of sfs-lsp's `--progress` lines. */
struct Progress
{
  /** Steps done. */
  int done = 0;
  /** Steps in all. */
  int total = 0;
  /** What it's doing: `scan`, `parse`, `index`, `edges` or `write`. */
  std::string phase;
};

/** The version of the export's schema this reads. */
inline constexpr int GRAPH_VERSION = 1;

/**
 * An edge kind's name, as the export writes it (`reads-system`).
 *
 * @param kind the kind
 * @return its name
 */
std::string_view edgeKindName(EdgeKind kind);

/**
 * A node kind's name, as the export writes it (`gameObject`).
 *
 * @param kind the kind
 * @return its name
 */
std::string_view nodeKindName(NodeKind kind);

/**
 * Read an export. Nodes and edges of kinds this doesn't know (a newer
 * sfs-lsp's) are left out, as are edges to nodes it doesn't have.
 *
 * @param json the export, as sfs-lsp writes it
 * @param error set to what's wrong when it can't be read
 * @return the graph, or nullopt if it isn't JSON, isn't an export, or is of
 *         another schema version
 */
std::optional<Graph> parseGraph(std::string_view json, std::string& error);

/**
 * The node a project file is of: the type declared in it or in the file
 * beside it of the same name (a .cpp is its .h's), the one named after the
 * file if several are, else the first; else the utils node made of it; else
 * the node whose code is written in it the most (a type split over several
 * .cpp files). The file may be named in any form that ends in its path from
 * the project root (absolute, Windows or WSL, either slash); case is ignored.
 *
 * @param graph the graph
 * @param path the file
 * @return its node, or nullopt if no node is of it
 */
std::optional<uint32_t> nodeOfFile(const Graph& graph, std::string_view path);

/**
 * Read one of sfs-lsp's `--progress` lines: `progress <done> <total> <phase>`.
 *
 * @param line the line, without its newline
 * @return its progress, or nullopt if it isn't a progress line
 */
std::optional<Progress> parseProgress(std::string_view line);

} // namespace architecture
