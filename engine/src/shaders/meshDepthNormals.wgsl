// Ambient occlusion's first pass (see ambientOcclusion.wgsl): what's drawn
// opaque, at half the viewport's size, as its world normal (xyz) and its depth
// along the view (w; 0 where nothing is). (meshCommon, meshSurface and the
// material's surface are prefixed.) Back faces are culled, as the lit pass
// culls them.

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
  var out: DepthOut;
  out.clip = frame.viewProjection * world;
  out.uv = instanceUv(vertex, instance);
  out.tintAlpha = instance.tint.a;
  out.worldPosition = world.xyz;
  out.normal = normalMatrix(model) * vertex.normal;
  out.vertexColor = vertex.color;
  return out;
}

@fragment
fn fs_main(in: DepthOut) -> @location(0) vec4f {
  let normal = normalize(in.normal);
  // Cut-out materials only where they're solid, as the lit pass draws them.
  // (The mode is the material's: the same across the draw.)
  if (surfaceAlphaMode() == ALPHA_MASK) {
    let s = surface(surfaceInput(in.uv, in.worldPosition, normal, in.vertexColor));
    if (s.alpha * in.tintAlpha < s.alphaClip) {
      discard;
    }
  }
  let depth = dot(in.worldPosition - frame.cameraPosition.xyz, frame.cameraForward.xyz);
  return vec4f(normal, max(depth, 1e-4));
}
