// Skinning: a skinned mesh's copy deformed by its pose, a vertex a thread,
// before the frame's passes draw it (all of them read the deformed copy as
// any mesh's vertices). Each vertex moves by the weighted sum of its joints'
// palette matrices (inverse(mesh) * joint * inverse bind), its normal by the
// same (no shear in a pose, so no inverse transpose needed).

// The dispatch's: how many vertices, where its palette starts among the
// palettes, and how many joints it has.
struct Skinning {
  vertexCount: u32,
  paletteOffset: u32,
  joints: u32,
  pad: u32,
}

@group(0) @binding(0) var<storage, read> palettes: array<mat4x4f>;
@group(0) @binding(1) var<uniform> skinning: Skinning;

// MeshVertex (11 floats: position, normal, colour, uv), as bound; SkinWeights
// (6 words: four 16-bit joints, four weights); the copy's vertices.
@group(1) @binding(0) var<storage, read> source: array<f32>;
@group(1) @binding(1) var<storage, read> weights: array<u32>;
@group(1) @binding(2) var<storage, read_write> skinned: array<f32>;

fn joint(index: u32) -> mat4x4f {
  return palettes[skinning.paletteOffset + min(index, skinning.joints - 1u)];
}

@compute @workgroup_size(64)
fn cs_main(@builtin(global_invocation_id) id: vec3u) {
  let v = id.x;
  if (v >= skinning.vertexCount || skinning.joints == 0u) {
    return;
  }
  let s = v * 11u;
  let w = v * 6u;
  let position = vec3f(source[s], source[s + 1u], source[s + 2u]);
  let normal = vec3f(source[s + 3u], source[s + 4u], source[s + 5u]);
  let packed = vec2u(weights[w], weights[w + 1u]);
  let joints = vec4u(packed.x & 0xffffu, packed.x >> 16u,
                     packed.y & 0xffffu, packed.y >> 16u);
  let weight = vec4f(bitcast<f32>(weights[w + 2u]), bitcast<f32>(weights[w + 3u]),
                     bitcast<f32>(weights[w + 4u]), bitcast<f32>(weights[w + 5u]));
  let total = weight.x + weight.y + weight.z + weight.w;

  var p = position;
  var n = normal;
  // A vertex without weights stays as bound.
  if (total > 1e-6) {
    let m = (joint(joints.x) * weight.x + joint(joints.y) * weight.y +
             joint(joints.z) * weight.z + joint(joints.w) * weight.w) *
            (1.0 / total);
    p = (m * vec4f(position, 1.0)).xyz;
    let turned = (m * vec4f(normal, 0.0)).xyz;
    let length2 = dot(turned, turned);
    n = select(normal, turned * inverseSqrt(length2), length2 > 1e-12);
  }
  skinned[s] = p.x;
  skinned[s + 1u] = p.y;
  skinned[s + 2u] = p.z;
  skinned[s + 3u] = n.x;
  skinned[s + 4u] = n.y;
  skinned[s + 5u] = n.z;
  // Colour and uv as they are.
  for (var i = 6u; i < 11u; i = i + 1u) {
    skinned[s + i] = source[s + i];
  }
}
