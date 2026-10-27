// Sun shadow-map pass: depth only, from the light's view-projection.
// (mesh3DCommon.wgsl is prefixed.)

@vertex
fn vs_main(vertex: VertexIn, instance: InstanceIn) -> @builtin(position) vec4f {
  let world = instanceModel(instance) * vec4f(vertex.position, 1.0);
  return frame.lightViewProjection * world;
}
