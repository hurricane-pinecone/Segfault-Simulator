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
  let placed = surfaceVertex(SurfaceVertex(world.xyz, normalMatrix(model) * vertex.normal,
                                            instanceUv(vertex, instance), 1.0));
  var out: ShadowOut;
  out.clip = sunCascade.viewProjection * vec4f(placed.position, 1.0);
  out.uv = placed.uv;
  out.tintAlpha = instance.tint.a * placed.keep;
  out.worldPosition = placed.position;
  out.normal = placed.normal;
  out.vertexColor = vertex.color;
  return out;
}

// Masked and see-through surfaces (and faded objects) cast only where solid:
// alpha at least the cut-out threshold, or a half for see-through ones.
fn cutOut(in: ShadowOut) {
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
}

@fragment
fn fs_main(in: ShadowOut) {
  cutOut(in);
}

// Rain's overhead map (drawn from straight above, see RainMap): as the
// shadow, and the height of what's drawn beside its depth (OpenGL can't read
// a depth texture's values: see rainSurfaceHeight()), as its bits: never 0,
// which is nothing drawn (a height of 0 is -0's).
@fragment
fn fs_rainMap(in: ShadowOut) -> @location(0) vec4u {
  cutOut(in);
  let bits = bitcast<u32>(in.worldPosition.z);
  return vec4u(select(bits, 0x80000000u, bits == 0u), 0u, 0u, 0u);
}
