// Point-light shadow pass: one face of a light's cube shadow map. Instead of
// the projection's depth, each texel keeps how far the nearest surface in its
// direction is from the light, as a fraction of the light's reach, so the lit
// pass compares distances whichever face it reads.
// (meshCommon, meshSurface and the material's surface are prefixed.)

struct ShadowFace {
  viewProjection: mat4x4f, // the face's view from the light
  light: vec4f,            // xyz the light's position, w 1 / its reach
}

@group(0) @binding(1) var<uniform> face: ShadowFace;

struct PointShadowOut {
  @builtin(position) clip: vec4f,
  @location(0) uv: vec2f,
  @location(1) tintAlpha: f32,
  @location(2) worldPosition: vec3f,
  @location(3) normal: vec3f,
  @location(4) vertexColor: vec3f,
}

@vertex
fn vs_main(vertex: VertexIn, instance: InstanceIn) -> PointShadowOut {
  let model = instanceModel(instance);
  let world = model * vec4f(vertex.position, 1.0);
  let placed = surfaceVertex(SurfaceVertex(world.xyz, normalMatrix(model) * vertex.normal,
                                            instanceUv(vertex, instance), 1.0));
  var out: PointShadowOut;
  out.clip = face.viewProjection * vec4f(placed.position, 1.0);
  out.uv = placed.uv;
  out.tintAlpha = instance.tint.a * placed.keep;
  out.worldPosition = placed.position;
  out.normal = placed.normal;
  out.vertexColor = vertex.color;
  return out;
}

// Masked and see-through surfaces (and faded objects) cast only where solid,
// as in the sun's shadow pass.
@fragment
fn fs_main(in: PointShadowOut) -> @builtin(frag_depth) f32 {
  let mode = surfaceAlphaMode();
  if (mode == ALPHA_ADDITIVE) {
    discard; // light, not a thing: it casts no shadow
  }
  // An opaque surface needn't be asked: only a faded object's tint cuts it.
  var alpha = in.tintAlpha;
  var cutoff = 0.5;
  if (mode != ALPHA_OPAQUE) {
    let solid = surfaceAlpha(surfaceInput(in.uv, in.worldPosition, normalize(in.normal), in.vertexColor));
    alpha = solid.x * in.tintAlpha;
    cutoff = select(0.5, solid.y, mode == ALPHA_MASK);
  }
  let cutsOut = mode != ALPHA_OPAQUE || in.tintAlpha < 1.0;
  if (cutsOut && alpha < cutoff) {
    discard;
  }
  return clamp(distance(in.worldPosition, face.light.xyz) * face.light.w, 0.0, 1.0);
}
