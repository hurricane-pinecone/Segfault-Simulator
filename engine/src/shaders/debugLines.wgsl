// Debug lines (DebugLines, IRenderer::drawLines): every line of the frame in
// one draw, last in the lit pass, a pixel wide, behind what's in front (depth
// tested, not written). Not lit: each in its colour as seen, divided by the
// frame's exposure (the grading's, and auto exposure's correction as last
// metered) so the composite gives it back about as given. Nothing in the
// glow (it doesn't bloom). (meshCommon and skyCommon are prefixed: the
// frame's view.)
//
// Each pixel's alpha takes how much a line covers it (1 - that), which
// temporal anti-aliasing reads: lines stay sharp, not smeared by its history.

struct DebugLineParams {
  scale: vec4f, // x 1 / the grading's exposure, y 1 if graded (auto exposure applies)
}

@group(1) @binding(0) var debugLineExposure: texture_2d<f32>;
@group(1) @binding(1) var<uniform> debugLine: DebugLineParams;

struct DebugLineOut {
  @builtin(position) clip: vec4f,
  @location(0) @interpolate(flat) color: vec4f,
}

@vertex
fn vs_debugLine(@location(0) position: vec3f, @location(1) color: vec4f) -> DebugLineOut {
  var out: DebugLineOut;
  out.clip = frame.viewProjection * vec4f(position, 1.0);
  out.color = vec4f(pow(color.rgb, vec3f(2.2)), color.a);
  return out;
}

struct DebugLineTargets {
  @location(0) color: vec4f,
  @location(1) glow: vec4f,
}

@fragment
fn fs_debugLine(in: DebugLineOut) -> DebugLineTargets {
  let metered = exp2(textureLoad(debugLineExposure, vec2i(0), 0).r);
  let correction = select(1.0, metered, debugLine.scale.y > 0.5);
  var out: DebugLineTargets;
  out.color = vec4f(in.color.rgb * debugLine.scale.x / correction, in.color.a);
  out.glow = vec4f(0.0);
  return out;
}
