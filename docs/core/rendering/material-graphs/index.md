# Material graphs

A material graph works out a material's surface (colour, opacity, metalness,
roughness and emission) from a graph of nodes. The editor builds graphs with
its [Material Editor](../../../editor/material-graphs/index.md). This page
covers the engine side: the graph model, how it compiles to a shader, and how
to add node types.

| Piece | Header | What it is |
| --- | --- | --- |
| `materialGraph::Graph` | `engine/core/rendering/materialGraph.h` | The nodes and links, and their JSON |
| `materialGraph::NodeType` | `engine/core/rendering/materialNodes.h` | Every kind of node: its pins, WGSL and CPU version |
| `materialGraph::compile()` | `engine/core/rendering/materialGraphCompiler.h` | Turns a graph into a `MaterialShader` and its values |
| `MeshMaterial::shader` | `engine/core/rendering/mesh.h` | A material drawn with a compiled graph |

## The graph

A `Graph` holds `Node`s and `Link`s. Every graph ends in one Material Output
node (type id `"output"`), whose inputs are the surface. Edit graphs through
its functions, which keep links valid:

```cpp
#include "engine/core/rendering/materialGraph.h"
#include "engine/core/rendering/materialNodes.h"

namespace mg = sfs::materialGraph;

mg::Graph graph;
const mg::NodeId output = graph.add(mg::kOutputType, {0, 0});
const mg::NodeId noise = graph.add("noise", {-300, 0});
const mg::NodeId mix = graph.add("lerp", {-150, 0});
graph.find(mix)->inputs[0] = {0.2f, 0.4f, 0.9f, 1.0f}; // A
graph.find(mix)->inputs[1] = {1.0f, 0.8f, 0.2f, 1.0f}; // B
graph.connect(noise, 0, mix, 2);                  // Noise.Out -> Mix.T
graph.connect(mix, 0, output, mg::kBaseColor);    // Mix.Out -> Base Colour
```

- `add()` makes a node of a known type id, its inputs holding their defaults.
  It returns 0 for an unknown type.
- `connect()` links an output to an input, replacing that input's link. It
  refuses links between missing pins, from a node to itself, and loops.
- `remove()` takes a node out with its links. `disconnect()` cuts an input's
  link.

Each `Node` has its inputs' own values (`inputs`, used while nothing is linked
to them), and the value its type edits on itself: a Colour or Number node's
`value`, or a Texture node's `texture` (`"asset:<id>"`) and `dataTexture`.

`Graph::toJson()` and `fromJson()` store a graph with pins named, not numbered,
so node types can gain pins without breaking saved graphs. Loading keeps nodes
of unknown types (made by a newer version, say) with their positions, and drops
links that would make a loop or an input's second link.

## In a material file

A node-graph material is a `MaterialAsset` with a `graph`. Its `.mat` file
(format version 2) holds the graph and the alpha mode, and none of the
standard material's fields:

```json
{
  "formatVersion": 2,
  "alphaMode": "opaque",
  "graph": {
    "nodes": [
      {"id": 1, "type": "output", "position": [0, 0], "inputs": {"Roughness": [0.6, 0, 0, 0]}},
      {"id": 2, "type": "color", "position": [-300, 0], "value": [1, 0.5, 0.2, 1]}
    ],
    "links": [{"from": [2, "Colour"], "to": [1, "Base Colour"]}]
  }
}
```

`MaterialAsset::toGraph()` turns a standard material into a graph that draws
the same, which is how the editor converts one.

## Types

Pins carry numbers (`ValueType::Float`) or vectors of 2 to 4 numbers. Any
output can feed any input; the value converts on the way:

- A number fills every component of a wider input.
- A wider value drops its last components.
- A narrower one gains zeros, and an alpha of 1 when it becomes a Vector 4.

Most maths nodes have dynamic inputs (`InputPin::type` empty). They take the
widest type linked to any of them, or a number when nothing is. The compiler
reports each node's dynamic type in `CompiledGraph::dynamicTypes`.

## Compiling

`compile()` orders the nodes that feed the Material Output (nodes feeding
nothing are left out) and writes a WGSL function for them:

```cpp
#include "engine/core/rendering/materialGraphCompiler.h"

const mg::CompiledGraph compiled = mg::compile(graph);
if (!compiled.shader)
  for (const mg::CompileError& error : compiled.errors)
    LOG_ERROR(error.message); // error.node is the node it's about (0: the graph)
```

The result has four parts:

- `shader`: a `MaterialShader`, null if the graph has errors. Its `wgsl()`
  declares the material's bindings and `fn surface(in: SurfaceIn) -> Surface`.
- `parameters`: the values the shader reads, as vec4s. These are every
  Colour and Number node's value, and every unlinked input's own value.
- `textures`: the images the shader reads, one per Texture node.
- `errors`: what's wrong. That can be no Material Output, a second one, a loop,
  or an unknown node type feeding the output.

Values are parameters rather than constants in the code, so graphs that differ
only in their values compile to the same WGSL. The renderer builds pipelines
once per WGSL and shares them, and changing a value never recompiles anything.

`MaterialShader::evaluate()` works out the same surface on the CPU, for a
`SurfacePoint` (texture coordinates, position, normal and texture directions,
view direction, vertex colour, time). The editor's software previews (the
Assets panel's thumbnails) use it. Every node has both a WGSL version and a C++
version, written side by side, and a test draws every node's outputs on a GPU
and compares them with the CPU's, pixel by pixel.

`MaterialShader::reads(output)` says what of the point one of the Material
Output's inputs depends on, through every node feeding it, as `PointInputs`
flags (UV, position, normal, tangent frame, view direction, vertex colour,
time). With none, it's the same all over a surface, so one evaluation gives
it. With none of `kPlacedInputs`, it's the same wherever the object is. The
render system uses this to work out a material's emission lighting once a
frame per mesh and material where it can (see
[Emission lights](../../../runtime/rendering/meshes-and-lighting/index.md#emission-lights)).

## Normals

The Material Output's Normal is in tangent space, as a normal map stores it:
(0, 0, 1) is the mesh's own normal, +x leans along the point's tangent and +y
along its bitangent.

Each point has a tangent frame (`SurfaceIn::tangent` and `bitangent` in WGSL,
`SurfacePoint` on the CPU): unit, at right angles to each other and to the
normal. The tangent runs along the texture's U and the bitangent up the image
(-V, since UV (0, 0) is the image's top left), mirrored with the texture. Meshes
carry no tangents: the passes work out which way the texture runs from
screen-space derivatives of position and UV, solving with the UV determinant's
sign so the frame is right whichever way the screen's axes run. Where a mesh
has no usable UVs, the frame comes from the world's axes instead (on the
ground, +X and +Y; on walls, the bitangent up +Z), so tangent-space normals
still work: Triplanar Normal Map turns its world-space bumps into this frame.

`tangentFrame()`, `textureDirections()` (a triangle's directions) and
`perturbNormal()` in `materialNodes.h` do the same on the CPU, and a GPU test
checks the two frames agree, with UVs and without.

The Normal Map node reads its image as data, and reads a flat normal with no
image set: a node type's `missingTexture` is what its texture reads unset, and
renderers bind a 1x1 texture of it (`MaterialShader::missingTexture()`).

## Drawing with a graph

A `MeshMaterial` with a `shader` is drawn by its graph: `shaderParameters` and
`shaderTextures` hold its values and images, in the shader's order. Only its
`alphaMode` is read from the standard fields.

```cpp
sfs::MeshMaterial material;
material.shader = compiled.shader;
material.shaderParameters = compiled.parameters;
material.shaderTextures = {/* each TextureSlot's image, in order */};
renderSystem.meshes().setMaterial("my-material", material);
```

`AssetStore::material()` does this for `.mat` files, and
`AssetStore::makeMaterial()` for a `MaterialAsset` in memory (one being edited,
say). Both load the textures and read data textures linearly. A graph that
doesn't compile draws magenta, with its errors logged.

The WebGPU renderer composes each pass's shader from the shared declarations,
the surface interface (`meshSurface.wgsl`), the material's surface and the
pass itself. The standard material is just another surface
(`meshStandardSurface.wgsl`). A surface that fails to build is logged once,
and its materials draw magenta. See
[Meshes, materials & lighting](../../../runtime/rendering/meshes-and-lighting/index.md#materials).

## Nodes

| Category | Nodes |
| --- | --- |
| Output | Material Output |
| Input | Colour, Number, Texture, Normal Map, Triplanar Texture, Triplanar Normal Map, UV, Vertex Colour, Position, Normal, View Direction, Time, Grass |
| Math | Add, Subtract, Multiply, Divide, Power, Minimum, Maximum, Mix, Clamp, Saturate, One Minus, Negate, Absolute, Fraction, Floor, Posterize, Square Root, Sine, Cosine, Step, Smoothstep, Remap |
| Vector | Split, Combine, Dot Product, Cross Product, Length, Distance, Normalize |
| UV | Tiling and Offset |
| Patterns | Noise, Voronoi, Checkerboard |
| Effects | Fresnel |

`nodeTypes()` lists them all, with their pins and descriptions. The type ids
stored in files (`"multiply"`, `"tilingOffset"`, ...) never change once
released.

## Adding a node type

Node types live in `makeNodeTypes()` in `materialNodes.cpp`. A type needs:

- A type id, label, category and description. The editor's Add Node menu shows
  the last three.
- Its inputs and outputs. Give an input no type to make it dynamic, and a
  `fallback` to read the mesh's UV, normal or view direction while unlinked.
- `emit`, which returns each output's WGSL expression from its inputs'. It
  can declare a local first (as the Texture node does), and shared functions
  go in `helpers`.
- `evaluate`, which works out the same outputs in C++. It must agree with
  `emit`.
- `reads`: the parts of the `SurfacePoint` its code reads itself, as
  `PointInputs` flags. Inputs' fallbacks are tracked for you; set this only
  for a node that reads the point directly (UV, Position, Time and the like).

Tests cover every node type automatically. `materialGraphTests` compiles each
output feeding the Material Output, and checks it changes only with the parts
of the point `reads()` says it depends on. `webGpuRendererTests` builds and draws
each one on a real GPU device, catching WGSL that doesn't compile, and checks
the GPU draws what `evaluate` works out (it skips where there's no adapter).

`materialRecipeTests` pastes every graph on the editor's
[recipes page](../../../editor/material-graphs/recipes/index.md) and checks it
loads whole and compiles, so the tutorials can't drift from the nodes.
