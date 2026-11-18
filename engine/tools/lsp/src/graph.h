#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sfs::lsp
{

/** The architecture export's JSON schema version (its "version"). */
inline constexpr int GRAPH_VERSION = 1;

/**
 * Told how an architecture export is going: `done` steps of `total`, in
 * `phase` ("scan", "parse", "index", "edges", "write"). `total` is known from
 * the first call; `done` never goes down and ends at `total`.
 */
using GraphProgress = std::function<
    void(std::size_t done, std::size_t total, std::string_view phase)>;

/**
 * The architecture of a tree as JSON (sfs-lsp --graph; the schema is in
 * docs/editor/sfs-lsp): its systems, services, game objects, components,
 * scene data, events, scriptable objects and utils files as nodes, and what
 * each one's code does with the others as edges, each with every site it's
 * written at. The types are read from the project's scripts (a root with a
 * project.sfsproj) and from the files given; the edges from the files given.
 * Nodes are sorted by id, edges by their ends and kind, sites by place, so
 * the same tree always gives the same JSON.
 *
 * @param root the root: file paths in the JSON are relative to it
 * @param files the C++ files to export, sorted, each once
 * @param progress told of each step; may be empty
 * @return the JSON
 */
nlohmann::ordered_json exportGraph(const std::filesystem::path& root,
                                   std::span<const std::filesystem::path> files,
                                   const GraphProgress& progress = {});

/**
 * Writes the architecture of the files from the command line as JSON on
 * stdout (sfs-lsp --graph). With --progress, writes `progress <done> <total>
 * <phase>` lines to stderr as it goes, each flushed.
 *
 * @param args the arguments after --graph: [--progress] [--root DIR] PATH...
 * @return 0; 2 for bad arguments
 */
int runGraph(const std::vector<std::string>& args);

} // namespace sfs::lsp
