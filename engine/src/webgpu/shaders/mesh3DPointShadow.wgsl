// Point-light shadow pass: one face of a light's cube shadow map. Instead of
// the projection's depth, each texel keeps how far the nearest surface in its
// direction is from the light, as a fraction of the light's reach, so the lit
// pass compares distances whichever face it reads.
// (mesh3DCommon, mesh3DSurface and the material's surface are prefixed.)

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
  var out: PointShadowOut;
  out.clip = face.viewProjection * world;
  out.uv = vertex.uv;
  out.tintAlpha = instance.tint.a;
  out.worldPosition = world.xyz;
  out.normal = normalMatrix(model) * vertex.normal;
  out.vertexColor = vertex.color;
  return out;
}

// Masked and see-through surfaces (and faded objects) cast only where solid,
// as in the sun's shadow pass.
@fragment
fn fs_main(in: PointShadowOut) -> @builtin(frag_depth) f32 {
  let s = surface(surfaceInput(in.uv, in.worldPosition, normalize(in.normal), in.vertexColor));
  let mode = surfaceAlphaMode();
  let alpha = select(s.alpha * in.tintAlpha, in.tintAlpha, mode == ALPHA_OPAQUE);
  let cutoff = select(0.5, s.alphaClip, mode == ALPHA_MASK);
  let cutsOut = mode != ALPHA_OPAQUE || in.tintAlpha < 1.0;
  if (cutsOut && alpha < cutoff) {
    discard;
  }
  return clamp(distance(in.worldPosition, face.light.xyz) * face.light.w, 0.0, 1.0);
}
