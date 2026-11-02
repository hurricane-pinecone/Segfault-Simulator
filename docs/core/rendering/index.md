# Rendering data

The plain data the renderer draws, in engine-core (`engine/core/rendering`).
It needs no GPU, so it can be built, combined and tested anywhere. Every style
of game uses it: a flat or isometric game's sprites, particles and voxel
terrain are meshes and materials too, just placed on its plane.

- **Meshes:** `MeshData` (vertices, indices and material sections, in
  `mesh.h`) and the primitives in `meshPrimitives.h` (boxes, spheres,
  cylinders and more).
- **Materials:** `MeshMaterial`, the standard glTF metallic-roughness
  surface, and `MaterialAsset` (`material.h`), the `.mat` file that names its
  textures by asset id.
- **Material graphs:** materials whose surface is worked out by a graph of
  nodes, compiled to a WGSL shader. See [Material graphs](./material-graphs/index.md).
- **Camera:** `Camera` and `Ray` (`camera.h`): an orbit camera for a Z-up
  world, with perspective or orthographic projection, and the ray through a
  pixel for picking.
- **Lighting:** `SceneLighting` (the sun, the moon, ambient light, sky colour
  and cloud cover),
  `PointLight`, and the settings for shadows, bloom, depth of field, haze and
  wind (`lighting.h`).
- **Grass:** the ground grass grows on, its painted maps and trampling
  (`grass.h`).
- **Culling and lights:** `Frustum` and `Aabb` for culling, the light
  clusters (`lightClusters.h`), and the lights a glowing material gives off
  (`emissionLights.h`).

Read more: [Material graphs](./material-graphs/index.md),
[Meshes, materials & lighting](../../runtime/rendering/meshes-and-lighting/index.md)
(drawing all of this).
