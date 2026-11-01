// The lit pass's vertex stage for meshes (after meshLit.wgsl): each vertex
// placed by its instance's model matrix.

@vertex
fn vs_main(vertex: VertexIn, instance: InstanceIn) -> VertexOut {
  let model = instanceModel(instance);
  let world = model * vec4f(vertex.position, 1.0);
  let placed = surfaceVertex(SurfaceVertex(world.xyz, normalMatrix(model) * vertex.normal,
                                            instanceUv(vertex, instance), 1.0));

  var out: VertexOut;
  out.clip = frame.viewProjection * vec4f(placed.position, 1.0);
  out.worldPosition = placed.position;
  out.normal = placed.normal;
  out.vertexColor = vertex.color;
  out.uv = placed.uv;
  out.tint = vec4f(instance.tint.rgb, instance.tint.a * placed.keep);
  out.grass = vec4f(1.0, 0.0, 0.0, 0.0);
  out.grassData = vec4f(0.0);
  return out;
}
