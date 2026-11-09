// The lit pass's cut-out depth: masked surfaces (leaves, a fence's wire)
// drawn depth only, cut out where they aren't solid, before anything's lit,
// so the lit pass then shades each of their pixels once (testing for equal
// depth, writing none) however many leaf cards lie over each other.
// (meshCommon, meshSurface and the material's surface are prefixed.) Placed
// exactly as meshLitVertex.wgsl places them: the same sums, invariant.

struct CutOutOut {
  @builtin(position) @invariant clip: vec4f,
  @location(0) uv: vec2f,
  @location(1) tintAlpha: f32,
  @location(2) worldPosition: vec3f,
  @location(3) normal: vec3f,
  @location(4) vertexColor: vec3f,
  @location(5) fade: f32, // its instance's dithered fade (see SurfaceIn)
}

@vertex
fn vs_main(vertex: VertexIn, instance: InstanceIn) -> CutOutOut {
  let model = instanceModel(instance);
  let world = model * vec4f(vertex.position, 1.0);
  let placed = surfaceVertex(SurfaceVertex(world.xyz, normalMatrix(model) * vertex.normal,
                                            instanceUv(vertex, instance), 1.0, vertex.color,
                                            model[3].xyz));
  var out: CutOutOut;
  out.clip = frame.viewProjection * vec4f(placed.position, 1.0);
  out.uv = placed.uv;
  out.tintAlpha = instance.tint.a * placed.keep;
  out.worldPosition = placed.position;
  out.normal = placed.normal;
  out.vertexColor = vertex.color;
  out.fade = instance.emission.w;
  return out;
}

// Cut out where the lit pass would cut it (its alpha against the threshold).
@fragment
fn fs_main(in: CutOutOut, @builtin(front_facing) front: bool) {
  let normal = normalize(select(-in.normal, in.normal, front || !surfaceFlipsBackFaces()));
  var point = surfaceInput(in.uv, in.worldPosition, normal, in.vertexColor);
  point.fade = in.fade;
  point.pixel = in.clip.xy;
  let solid = surfaceAlpha(point);
  if (solid.x * in.tintAlpha < solid.y) {
    discard;
  }
}
