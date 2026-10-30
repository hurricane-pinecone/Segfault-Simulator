// The lit pass's vertex stage for meshes (after mesh3DLit.wgsl): each vertex
// placed by its instance's model matrix.

@vertex
fn vs_main(vertex: VertexIn, instance: InstanceIn) -> VertexOut {
  let model = instanceModel(instance);
  let world = model * vec4f(vertex.position, 1.0);

  var out: VertexOut;
  out.clip = frame.viewProjection * world;
  out.worldPosition = world.xyz;
  out.normal = normalMatrix(model) * vertex.normal;
  out.vertexColor = vertex.color;
  out.uv = instanceUv(vertex, instance);
  out.tint = instance.tint;
  out.grass = vec4f(1.0, 0.0, 0.0, 0.0);
  out.grassData = vec4f(0.0);
  return out;
}
