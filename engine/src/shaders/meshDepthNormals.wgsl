// Ambient occlusion's first pass (see ambientOcclusion.wgsl): what's drawn
// opaque, at half the viewport's size, as its world normal (xyz) and its depth
// along the view (w; 0 where nothing is). (meshCommon, meshSurface and the
// material's surface are prefixed.) Back faces are culled, as the lit pass
// culls them (but a double-sided material's).

struct DepthOut {
  @builtin(position) clip: vec4f,
  @location(0) uv: vec2f,
  @location(1) tintAlpha: f32,
  @location(2) worldPosition: vec3f,
  @location(3) normal: vec3f,
  @location(4) vertexColor: vec3f,
}

@vertex
fn vs_main(vertex: VertexIn, instance: InstanceIn) -> DepthOut {
  let model = instanceModel(instance);
  let world = model * vec4f(vertex.position, 1.0);
  let placed = surfaceVertex(SurfaceVertex(world.xyz, normalMatrix(model) * vertex.normal,
                                            instanceUv(vertex, instance), 1.0));
  var out: DepthOut;
  out.clip = frame.viewProjection * vec4f(placed.position, 1.0);
  out.uv = placed.uv;
  out.tintAlpha = instance.tint.a * placed.keep;
  out.worldPosition = placed.position;
  out.normal = placed.normal;
  out.vertexColor = vertex.color;
  return out;
}

@fragment
fn fs_main(in: DepthOut, @builtin(front_facing) front: bool) -> @location(0) vec4f {
  // A back face (drawn only for a double-sided material) faces the view.
  let normal = normalize(select(-in.normal, in.normal, front));
  // Cut-out materials only where they're solid, as the lit pass draws them.
  // (The mode is the material's: the same across the draw.)
  if (surfaceAlphaMode() == ALPHA_MASK) {
    let solid = surfaceAlpha(surfaceInput(in.uv, in.worldPosition, normal, in.vertexColor));
    if (solid.x * in.tintAlpha < solid.y) {
      discard;
    }
  }
  let depth = dot(in.worldPosition - frame.cameraPosition.xyz, frame.cameraForward.xyz);
  return vec4f(normal, max(depth, 1e-4));
}
