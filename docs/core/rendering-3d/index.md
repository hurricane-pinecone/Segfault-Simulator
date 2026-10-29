# 3D rendering data

The plain data the 3D renderer draws, in engine-core (`engine/core/rendering/3d`).
It needs no GPU, so it can be built, combined and tested anywhere:

- **Meshes:** `MeshData` (vertices, indices and material sections) and the
  primitives in `meshPrimitives.h` (boxes, spheres, cylinders and more).
- **Materials:** `MeshMaterial`, the standard glTF metallic-roughness surface,
  and `MaterialAsset`, the `.mat` file that names its textures by asset id.
- **Material graphs:** materials whose surface is worked out by a graph of
  nodes, compiled to a WGSL shader.

Read more: [Material graphs](./material-graphs/index.md),
[3D rendering](../../runtime/rendering-3d/index.md) (drawing all of this).
