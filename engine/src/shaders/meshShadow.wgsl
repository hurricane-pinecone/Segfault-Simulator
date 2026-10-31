// Sun shadow-map pass: depth only, from the light's view-projection: the one
// map's, or the cascade being drawn's (each its own tile of the atlas).
// (meshCommon, meshSurface and the material's surface are prefixed.)

struct SunCascade {
  viewProjection: mat4x4f,
}

@group(0) @binding(1) var<uniform> sunCascade: SunCascade;

struct ShadowOut {
  @builtin(position) clip: vec4f,
  @location(0) uv: vec2f,
  @location(1) tintAlpha: f32,
  @location(2) worldPosition: vec3f,
  @location(3) normal: vec3f,
  @location(4) vertexColor: vec3f,
}

@vertex
fn vs_main(vertex: VertexIn, instance: InstanceIn) -> ShadowOut {
  let model = instanceModel(instance);
  let world = model * vec4f(vertex.position, 1.0);
  var out: ShadowOut;
  out.clip = sunCascade.viewProjection * world;
  out.uv = instanceUv(vertex, instance);
  out.tintAlpha = instance.tint.a;
  out.worldPosition = world.xyz;
  out.normal = normalMatrix(model) * vertex.normal;
  out.vertexColor = vertex.color;
  return out;
}

// Masked and see-through surfaces (and faded objects) cast only where solid:
// alpha at least the cut-out threshold, or a half for see-through ones.
@fragment
fn fs_main(in: ShadowOut) {
  let s = surface(surfaceInput(in.uv, in.worldPosition, normalize(in.normal), in.vertexColor));
  let mode = surfaceAlphaMode();
  if (mode == ALPHA_ADDITIVE) {
    discard; // light, not a thing: it casts no shadow
  }
  let alpha = select(s.alpha * in.tintAlpha, in.tintAlpha, mode == ALPHA_OPAQUE);
  let cutoff = select(0.5, s.alphaClip, mode == ALPHA_MASK);
  let cutsOut = mode != ALPHA_OPAQUE || in.tintAlpha < 1.0;
  if (cutsOut && alpha < cutoff) {
    discard;
  }
}
