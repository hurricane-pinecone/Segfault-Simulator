#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include "nlohmann/json.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sfs::materialGraph
{

/** A node's id within its graph. 0 is never used: it means "no node". */
using NodeId = uint32_t;

/**
 * A node: one step of the surface's recipe (see NodeType). Values it holds:
 * its inputs' own values (used while nothing is connected to them), and the
 * value, texture or name its type edits on the node itself.
 */
struct Node
{
  NodeId id = 0;
  std::string type;         // a NodeType's id, e.g. "multiply"
  glm::vec2 position{0.0f}; // where it sits on the canvas, in canvas units

  // Each input's own value, in the type's input order (as many as it has
  // inputs). Only the first components count, as many as the input's type
  // has.
  std::vector<glm::vec4> inputs;

  glm::vec4 value{0.0f}; // a Colour node's colour (linear) or a Number's value
  std::string texture;   // a Texture node's image, "asset:<id>" ("" for none)
  // A Texture node's image holds data (masks, roughness), not colour: it's
  // read as-is rather than converted from sRGB.
  bool dataTexture = false;

  bool operator==(const Node&) const = default;
};

/** A connection: a node's output feeding another node's input. */
struct Link
{
  NodeId fromNode = 0;
  uint32_t fromOutput = 0; // index into the source's outputs
  NodeId toNode = 0;
  uint32_t toInput = 0; // index into the destination's inputs

  bool operator==(const Link&) const = default;
};

/**
 * A material's node graph: how its surface (colour, opacity, metalness,
 * roughness, glow) is worked out for each point drawn. Every graph ends in one
 * Material Output node; compile() (materialGraphCompiler.h) turns it into a
 * shader.
 *
 * Plain data, edited through the functions below so links stay valid: each
 * input has at most one link, and links only join nodes in the graph.
 *
 * Stored in a .mat file as JSON, inputs and outputs by name (so node types can
 * gain pins without breaking saved graphs):
 *
 *   {
 *     "nodes": [
 *       {"id": 1, "type": "output", "position": [300, 0],
 *        "inputs": {"Roughness": [0.8, 0, 0, 0]}},
 *       {"id": 2, "type": "color", "position": [0, 0],
 *        "value": [1, 0.5, 0.2, 1]}
 *     ],
 *     "links": [{"from": [2, "Colour"], "to": [1, "Base Colour"]}]
 *   }
 */
struct Graph
{
  std::vector<Node> nodes;
  std::vector<Link> links;
  NodeId nextId = 1;

  /** A node by id; null if there's none. */
  Node* find(NodeId id);
  const Node* find(NodeId id) const;

  /**
   * Add a node of a known type (NodeType id) at a canvas position, its inputs
   * holding their defaults.
   *
   * @return its id, or 0 (nothing added) for an unknown type.
   */
  NodeId add(std::string_view type, glm::vec2 position);

  /** Remove a node and every link to or from it. */
  void remove(NodeId id);

  /**
   * Connect `from`'s output to `to`'s input, replacing whatever fed that input
   * before.
   *
   * @return false (nothing changed) if a node or pin doesn't exist, the link
   * would join a node to itself, or it would make a loop (a node feeding into
   * itself through others).
   */
  bool connect(NodeId from, uint32_t output, NodeId to, uint32_t input);

  /** Remove the link into a node's input, if it has one. */
  void disconnect(NodeId to, uint32_t input);

  /** The link into a node's input; null if nothing is connected to it. */
  const Link* linkInto(NodeId to, uint32_t input) const;

  /** Whether `from` feeds into `to`, through any chain of links. */
  bool feeds(NodeId from, NodeId to) const;

  /** The id of the graph's (first) Material Output node; 0 if it has none. */
  NodeId output() const;

  /**
   * A graph that looks like a fresh standard material: white (times the
   * mesh's vertex colours, as untextured models carry their palette in them),
   * matte, not metal.
   */
  static Graph makeDefault();

  nlohmann::json toJson() const;

  /**
   * Read a graph saved by toJson(). Nodes of unknown types are kept (with
   * their positions, so the graph still opens; compiling reports them), links
   * to pins that don't exist are dropped, and so are links that would make an
   * input's second link or a loop. Nothing if `json` isn't an object.
   */
  static std::optional<Graph> fromJson(const nlohmann::json& json);

  bool operator==(const Graph&) const = default;
};

} // namespace sfs::materialGraph
