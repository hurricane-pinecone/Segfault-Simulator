// What a material's surface function is written against (after mesh3DCommon):
// a surface (mesh3DStandardSurface.wgsl, or a compiled material graph, see
// materialGraphCompiler.h) declares its own bindings (group 1) and
//
//   fn surface(in: SurfaceIn) -> Surface
//   fn surfaceAlphaMode() -> u32   // ALPHA_OPAQUE, ALPHA_MASK or ALPHA_BLEND
//
// The passes call surface() before anything else, while control flow is still
// uniform, so it may sample textures. Must match AlphaMode.

const ALPHA_OPAQUE: u32 = 0u;
const ALPHA_MASK: u32 = 1u;
const ALPHA_BLEND: u32 = 2u;

// The point being drawn.
struct SurfaceIn {
  uv: vec2f,          // the mesh's texture coordinates
  position: vec3f,    // world space
  normal: vec3f,      // world space, unit length
  tangent: vec3f,     // the tangent frame (see tangentFrame): tangent space's +x
  bitangent: vec3f,   // and +y, unit, at right angles to each other and normal
  toView: vec3f,      // unit direction to the camera
  vertexColor: vec3f, // the mesh's own colour (before the object's tint)
  time: f32,          // seconds the scene has run
}

// How it looks. The object's tint multiplies baseColor and alpha afterwards.
struct Surface {
  baseColor: vec3f, // linear RGB
  alpha: f32,
  metallic: f32,    // 0 to 1
  roughness: f32,   // 0 (mirror) to 1 (matte)
  normal: vec3f,    // tangent space (a normal map's): (0, 0, 1) is the mesh's own
  emission: vec3f,  // light given off, added after lighting
  alphaClip: f32,   // ALPHA_MASK: cut out below this alpha
  bloom: f32,       // how much of its light blooms (1 normally; 0 none)
}

// A point's tangent frame, as the CPU's tangentFrame() builds it: tangent along
// the texture (+u) and bitangent up it (-v, as UV (0, 0) is the image's top
// left), unit and at right angles to each other and the normal; mirrored with
// the texture. Which way the texture runs comes from how position and UV
// change across the screen (solved keeping the UV determinant's sign, so it's
// right whichever way the screen's axes run), so meshes need no tangents.
// Without UVs, a frame from the world's axes. Uses derivatives: call it in
// uniform control flow. Returns (tangent, bitangent, normal).
fn tangentFrame(normal: vec3f, position: vec3f, uv: vec2f) -> mat3x3f {
  let dp1 = dpdx(position);
  let dp2 = dpdy(position);
  let duv1 = dpdx(uv);
  let duv2 = dpdy(uv);
  let orientation = sign(duv1.x * duv2.y - duv2.x * duv1.y);
  let alongU = (dp1 * duv2.y - dp2 * duv1.y) * orientation;
  let upImage = -(dp2 * duv1.x - dp1 * duv2.x) * orientation;
  let flatU = alongU - normal * dot(normal, alongU);
  let flatUp = upImage - normal * dot(normal, upImage);
  var tangent: vec3f;
  var bitangent: vec3f;
  if (dot(flatU, flatU) > 1e-20) {
    tangent = normalize(flatU);
    bitangent = cross(normal, tangent);
    if (dot(bitangent, flatUp) < 0.0) {
      bitangent = -bitangent; // a mirrored texture
    }
  } else if (dot(flatUp, flatUp) > 1e-20) {
    bitangent = normalize(flatUp);
    tangent = cross(bitangent, normal);
  } else {
    let reference = select(vec3f(0.0, 1.0, 0.0), vec3f(0.0, 0.0, 1.0), abs(normal.z) < 0.999);
    tangent = normalize(cross(reference, normal));
    bitangent = cross(normal, tangent);
  }
  return mat3x3f(tangent, bitangent, normal);
}

// The SurfaceIn for a point (normal already unit length).
fn surfaceInput(uv: vec2f, position: vec3f, normal: vec3f, vertexColor: vec3f) -> SurfaceIn {
  let frame3 = tangentFrame(normal, position, uv);
  var result: SurfaceIn;
  result.uv = uv;
  result.position = position;
  result.normal = normal;
  result.tangent = frame3[0];
  result.bitangent = frame3[1];
  result.toView = normalize(frame.cameraPosition.xyz - position);
  result.vertexColor = vertexColor;
  result.time = frame.time.x;
  return result;
}
