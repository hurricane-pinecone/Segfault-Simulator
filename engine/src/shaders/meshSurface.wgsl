// What a material's surface function is written against (after meshCommon):
// a surface (meshStandardSurface.wgsl, or a compiled material graph, see
// materialGraphCompiler.h) declares its own bindings (group 1) and
//
//   fn surface(in: SurfaceIn) -> Surface
//   fn surfaceAlphaMode() -> u32   // ALPHA_OPAQUE, ALPHA_MASK, ALPHA_BLEND or
//                                  // ALPHA_ADDITIVE
//
// The passes call surface() before anything else, while control flow is still
// uniform, so it may sample textures. Must match AlphaMode.
//
// A surface may also place its mesh's vertices itself (a deformed terrain's
// patch does), with
//
//   fn surfaceVertex(v: SurfaceVertex) -> SurfaceVertex
//
// which every pass's vertex stage calls with each vertex as placed; without
// one (shaders::meshPass() adds it), they stay where they are. And it may say
// how solid it is more cheaply than surface() would, for the passes that need
// nothing else (shadows, ambient occlusion's depth), with
//
//   fn surfaceAlpha(in: SurfaceIn) -> vec2f // x alpha, y the cut-out threshold
//
// (without one, surface()'s alpha and alphaClip). A surface whose normals
// mean the same from either side (foliage's, bent out from a crown) keeps
// them on back faces with
//
//   fn surfaceFlipsBackFaces() -> bool  // false (true without one)

const ALPHA_OPAQUE: u32 = 0u;
const ALPHA_MASK: u32 = 1u;
const ALPHA_BLEND: u32 = 2u;
const ALPHA_ADDITIVE: u32 = 3u; // light added to what's behind

// A vertex as placed in the world, for surfaceVertex(): it may move it, turn
// its normal, give it other texture coordinates, or drop it (`keep` 0: the
// triangles it's in are cut out where it's dropped, as a masked surface's
// are).
struct SurfaceVertex {
  position: vec3f, // world space
  normal: vec3f,   // world space
  uv: vec2f,
  keep: f32,       // 1 normally
  color: vec3f,    // the mesh's vertex colour (read only)
  origin: vec3f,   // where its instance stands: its model matrix's origin (read only)
}

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
  grass: vec4f,       // grass's blade: length, how far up it (0 root, 1 tip), random
  grassData: vec4f,   // grass's maps' Data channels where it grows
  fade: f32,          // its instance's dithered fade (MeshInstance::emission.w): 0 none, above 0 fading
                      // out, below 0 fading in (see ditherFaded())
  pixel: vec2f,       // where it is on the screen (pixels; 0 in passes that don't fade)
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
  water: f32,       // water standing on it, 0 to 1 (a terrain's puddles; 0 normally)
  soak: f32,        // how soaked it is besides the weather's wetness (a puddle's edge)
  translucency: f32, // sunlight let through from behind, 0 (none) to 1 (a thin leaf)
}

// Whether a point fading by dithering (SurfaceIn's fade) is cut out here: a
// fading-out instance keeps the pixels whose noise is above its fade, a
// fading-in one those below, so two levels crossing over cover each pixel
// once; the noise changes each frame, so temporal anti-aliasing blends them.
fn ditherFaded(fade: f32, pixel: vec2f) -> bool {
  if (fade == 0.0) {
    return false;
  }
  let noise = ditherNoise(pixel);
  return select(noise > -fade, noise < fade, fade > 0.0);
}

// Dithering's noise at a pixel, 0..1: interleaved gradient noise, shifted each
// frame (so temporal anti-aliasing blends what it cuts into a fade).
fn ditherNoise(pixel: vec2f) -> f32 {
  let step = f32(u32(frame.time.x * 60.0) % 64u);
  let p = pixel + vec2f(5.588238 * step);
  return fract(52.9829189 * fract(dot(p, vec2f(0.06711056, 0.00583715))));
}

// Whether a point of something that clears the camera's view
// (MeshMaterial::fadeNearCamera: `near` metres, above 0) is cut out here, by
// dithering: where the camera would be inside it (gone within `near` of the
// eye, all back by 1.5 times it), and, if the frame asks, round the line of sight to what the camera looks at (frame.cameraTarget:
// gone within 60% of its radius, back by all of it; not the last few metres
// before the target), so it never hides what's looked at. Only seen by the
// camera (in.pixel set): its shadow stays whole.
fn cameraFaded(in: SurfaceIn, near: f32) -> bool {
  if (near <= 0.0 || in.pixel.x <= 0.0) {
    return false;
  }
  let eye = frame.cameraPosition.xyz;
  var keep = smoothstep(near, near * 1.5, distance(in.position, eye));
  let radius = frame.cameraTarget.w;
  if (radius > 0.0) {
    let sight = frame.cameraTarget.xyz - eye;
    let length2 = max(dot(sight, sight), 1e-6);
    let t = clamp(dot(in.position - eye, sight) / length2, 0.0, 1.0);
    let off = distance(in.position, eye + sight * t);
    let reach = sqrt(length2);
    let before = 1.0 - smoothstep(reach - 3.0, reach - 1.0, t * reach);
    keep = min(keep, mix(1.0, smoothstep(radius * 0.6, radius, off), before));
  }
  return keep < 1.0 && ditherNoise(in.pixel) >= keep;
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
  // Not grass (grass's passes set their own): full length, at the root.
  result.grass = vec4f(1.0, 0.0, 0.0, 0.0);
  result.grassData = vec4f(0.0);
  result.fade = 0.0;
  result.pixel = vec2f(0.0);
  return result;
}
