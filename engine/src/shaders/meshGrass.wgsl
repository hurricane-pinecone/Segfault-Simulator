// The lit pass's vertex stage for grass (after meshLit.wgsl, whose fragment
// stage lights it like any surface, with the field's material).
//
// Each instance is one blade, as meshGrassGrow.wgsl grew it this frame,
// drawn as a triangle strip up it: two vertices a row for its segments' rows
// from the root, then the tip, along its curve (grassBlade.wgsl), tapering
// to a point.
//
// Before the blades, the ground they grow on is drawn again in their colour
// (vs_ground, fs_ground), as much as they cover it, out to a little past
// where they're drawn: so the gaps between blades look like grass.

// GrownBlade, Grass and the blade's shape: grassBlade.wgsl (before this).

// How shut in a point `t` up a blade `height` tall is by the blades around
// it (the lit pass's SurfaceIn::grass.w): most at the root, none at the tip,
// and less in short grass, which has less over it. It takes the sky's light
// there and some of the sun's, so a field looks deep at any distance, past
// where its blades' own shadows are drawn.
fn deepIn(height: f32, t: f32) -> f32 {
  let below = 1.0 - t;
  return grass.shadow.x * below * sqrt(below) * mix(0.35, 1.0, smoothstep(0.03, 0.3, height));
}

@group(2) @binding(0) var<storage, read> blades: array<GrownBlade>;
@group(2) @binding(1) var<uniform> grass: Grass;
@group(2) @binding(2) var trampleMap: texture_2d<f32>; // rg direction, b how flat
@group(2) @binding(3) var trampleSampler: sampler;
@group(2) @binding(4) var dataMap: texture_2d<f32>; // the maps' data channels
@group(2) @binding(5) var shapeMap: texture_2d<f32>; // r coverage, g length

@vertex
fn vs_main(@builtin(vertex_index) index: u32, @builtin(instance_index) instance: u32) -> VertexOut {
  let grown = blades[instance];
  let pressed = textureSampleLevel(trampleMap, trampleSampler,
                                   (grown.root.xy - grass.trample.xy) * grass.trample.zw, 0.0);
  let b = bladeAt(grown, grass.blade.x, pressed);
  let height = b.height;
  let shape = b.shape;
  let segments = b.segments;
  let across = b.across;
  let ground = b.ground;

  // Where up the blade this vertex is: a row's left or right, or the tip.
  let row = min(index / 2u, segments);
  let t = f32(row) / f32(segments);
  let side = select(f32(index % 2u) * 2.0 - 1.0, 0.0, index >= 2u * segments);

  let center = bladeCenter(b, t);
  let along = bladeAlong(b, t);
  // Full width low down, narrowing to a point.
  let position = center + across * (side * b.width * 0.5 * (1.0 - t * t));

  // Its face, turned to the camera (blades show both sides), rounded across
  // its width and softened toward the ground's, so a field shades smoothly.
  var normal = normalize(cross(across, along));
  if (dot(normal, frame.cameraPosition.xyz - position) < 0.0) {
    normal = -normal;
  }
  normal = normalize(normal + across * side * 0.3 + ground * 0.6);

  var out: VertexOut;
  out.clip = frame.viewProjection * vec4f(position, 1.0);
  out.worldPosition = position;
  out.normal = normal;
  // Lighter or darker, a little yellower or bluer: no two alike; and darker
  // down in the grass, where less light gets in.
  let light = 0.82 + shape.w * 0.3;
  let shift = (fract(shape.z * 7.31) - 0.5) * 0.16;
  out.vertexColor = vec3f(light * (1.0 + shift), light, light * (1.0 - shift)) * mix(0.55, 1.0, t);
  out.uv = vec2f(side * 0.5 + 0.5, 1.0 - t);
  out.tint = vec4f(1.0);
  // For its material (the Grass node): its length, how far up it, its own
  // random number, and the maps' data where it grows.
  out.grass = vec4f(height / max(grass.blade.w, 1e-4), t, shape.z, deepIn(height, t));
  out.grassData = textureSampleLevel(dataMap, trampleSampler, unpack2x16unorm(grown.look.w), 0.0);
  return out;
}

// The ground under the grass, as its mesh (buffer 0 only).
@vertex
fn vs_ground(in: VertexIn) -> VertexOut {
  let world = (grass.model * vec4f(in.position, 1.0)).xyz;
  var out: VertexOut;
  out.clip = frame.viewProjection * vec4f(world, 1.0);
  out.worldPosition = world;
  out.normal = normalize((grass.normalMatrix * vec4f(in.normal, 0.0)).xyz);
  // As a blade low down: shaded by the grass around it.
  out.vertexColor = vec3f(0.62);
  out.uv = vec2f(0.5, 0.95);
  out.tint = vec4f(1.0);
  out.grass = vec4f(1.0, 0.05, 0.5, 0.0); // fs_ground's, per point
  out.grassData = vec4f(0.0);
  return out;
}

@fragment
fn fs_ground(in: VertexOut) -> LitOut {
  // Its maps here: how much grass covers it, and (for the material, as the
  // blades see them) how long it is and its data.
  let local = (grass.inverseModel * vec4f(in.worldPosition, 1.0)).xy;
  let uv = (local - grass.maps.xy) * grass.maps.zw;
  let maps = textureSampleLevel(shapeMap, trampleSampler, uv, 0.0);
  var here = in;
  // Down between the blades, in among them; a field seen from afar shows
  // more of their tops, so a little less.
  let away = distance(in.worldPosition, frame.cameraPosition.xyz) / max(grass.blade.y, 1.0);
  let deep = deepIn(maps.g * grass.blade.w, mix(0.15, 0.45, smoothstep(0.2, 1.0, away)));
  here.grass = vec4f(maps.g, 0.05, 0.5, deep);
  here.grassData = textureSampleLevel(dataMap, trampleSampler, uv, 0.0);
  let lit = shade(here);
  // As much as grass covers it, where it faces up enough to grow any; fading
  // out just past where blades are drawn (beyond, the ground's own material
  // carries on: a terrain's meadow layers look like grass from afar, see
  // meshTerrainSurface.wgsl), so a big mesh's window of maps never shows.
  let coverage = maps.r;
  let up = smoothstep(0.45, 0.55, normalize(in.normal).z);
  let amount = coverage * up * grass.blade.z * (1.0 - smoothstep(1.0, 1.15, away));
  return LitOut(vec4f(lit.color.rgb, amount), vec4f(lit.glow.rgb, amount));
}
