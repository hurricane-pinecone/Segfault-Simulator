// For leaving out grass tiles hidden behind what's drawn opaque (see
// meshGrassGrow.wgsl's hiddenTile()): the farthest depth along the view of
// what's drawn over each block of the screen, from ambient occlusion's first
// pass (meshDepthNormals.wgsl: its depth, 0 where nothing is drawn), in
// levels (an R32Uint texture's mips: the depths' bits, which order as they
// do) each with blocks twice the size. The
// finest level's blocks are 2 x 2 of that pass's texels; each next level's
// are 2 x 2 of the level before's, and its last row and column take in the
// one left over before them, if there is one, so nothing's missed.

struct Params {
  reading: vec4i, // x 1 reading the depths and normals (for the finest
                  // level), 0 the level before
}

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var depths: texture_2d<f32>; // the depths and normals
@group(0) @binding(2) var before: texture_2d<u32>; // the level before

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4f {
  let corner = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
  return vec4f(corner * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
}

// Nothing drawn: farther than anything.
const kNothing = 1e30;

@fragment
fn fs_main(@builtin(position) position: vec4f) -> @location(0) vec4u {
  let block = vec2i(position.xy);
  let first = block * 2;
  // Its 2 x 2 (and the one left over past them at the end of a row or
  // column, if there is one).
  let firstLevel = params.reading.x == 1;
  let size = select(vec2i(textureDimensions(before)), vec2i(textureDimensions(depths)),
                    firstLevel);
  let ours = max(size / 2, vec2i(1));
  let last = min(first + 1 + vec2i(block == ours - 1) * (size - ours * 2), size - 1);
  var deepest = 0.0;
  for (var y = first.y; y <= last.y; y++) {
    for (var x = first.x; x <= last.x; x++) {
      if (firstLevel) {
        let depth = textureLoad(depths, vec2i(x, y), 0).w;
        deepest = max(deepest, select(depth, kNothing, depth <= 0.0));
      } else {
        deepest = max(deepest, bitcast<f32>(textureLoad(before, vec2i(x, y), 0).r));
      }
    }
  }
  return vec4u(bitcast<u32>(deepest), 0u, 0u, 0u);
}
