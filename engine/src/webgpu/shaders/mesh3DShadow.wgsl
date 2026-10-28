// Sun shadow-map pass: depth only, from the light's view-projection.
// (mesh3DCommon.wgsl is prefixed.)

struct ShadowOut {
  @builtin(position) clip: vec4f,
  @location(0) uv: vec2f,
  @location(1) tintAlpha: f32,
}

@vertex
fn vs_main(vertex: VertexIn, instance: InstanceIn) -> ShadowOut {
  let world = instanceModel(instance) * vec4f(vertex.position, 1.0);
  var out: ShadowOut;
  out.clip = frame.lightViewProjection * world;
  out.uv = vertex.uv;
  out.tintAlpha = instance.tint.a;
  return out;
}

// Masked and see-through surfaces (and faded objects) cast only where solid.
@fragment
fn fs_main(in: ShadowOut) {
  let textureAlpha = textureSample(baseTexture, materialSampler, materialUv(in.uv)).a;
  let alpha = surfaceAlpha(textureAlpha, in.tintAlpha);
  let cutsOut = material.mode.x != ALPHA_OPAQUE || in.tintAlpha < 1.0;
  if (cutsOut && alpha < alphaCutoff()) {
    discard;
  }
}
