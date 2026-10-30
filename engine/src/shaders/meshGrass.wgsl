// The lit pass's vertex stage for grass (after meshLit.wgsl, whose fragment
// stage lights it like any surface, with the field's material).
//
// Each instance is one blade, as meshGrassGrow.wgsl grew it this frame,
// drawn as a triangle strip up it: two vertices a row for its segments' rows
// from the root, then the tip. The blade is a curve from its root that bends
// over toward the sum of its own lean, the wind and any trampling, tapering
// to a point.
//
// Before the blades, the ground they grow on is drawn again in their colour
// (vs_ground, fs_ground), as much as they cover it: so the gaps between
// blades, and the ground past where they're drawn, look like grass.

struct GrownBlade {
  root: vec4f, // world root, height
  look: vec4u, // see meshGrassGrow.wgsl
}

struct Grass {
  trample: vec4f, // xy the trample map's world min, zw 1 / its world size
  blade: vec4f,   // x stiffness (0..1), y draw distance, z how much the ground's tinted, w full height
  maps: vec4f,    // xy the maps' grid min (own space), zw 1 / its size (0: unpainted)
  model: mat4x4f, // the ground's own space to the world
  normalMatrix: mat4x4f,
  inverseModel: mat4x4f,
}

@group(2) @binding(0) var<storage, read> blades: array<GrownBlade>;
@group(2) @binding(1) var<uniform> grass: Grass;
@group(2) @binding(2) var trampleMap: texture_2d<f32>; // rg direction, b how flat
@group(2) @binding(3) var trampleSampler: sampler;
@group(2) @binding(4) var dataMap: texture_2d<f32>; // the maps' data channels
@group(2) @binding(5) var shapeMap: texture_2d<f32>; // r coverage, g length

@vertex
fn vs_main(@builtin(vertex_index) index: u32, @builtin(instance_index) instance: u32) -> VertexOut {
  let blade = blades[instance];
  let root = blade.root.xyz;
  let height = blade.root.w;
  let tilt = unpack2x16snorm(blade.look.x);
  let ground = vec3f(tilt, sqrt(max(1.0 - dot(tilt, tilt), 0.0)));
  let sizes = unpack2x16float(blade.look.y);
  let width = sizes.x;
  let segments = u32(sizes.y);
  let shape = unpack4x8unorm(blade.look.z); // facing, lean, random, tone

  // Where up the blade this vertex is: a row's left or right, or the tip.
  let row = min(index / 2u, segments);
  let t = f32(row) / f32(segments);
  let side = select(f32(index % 2u) * 2.0 - 1.0, 0.0, index >= 2u * segments);

  // Which way it faces, and across it; up is mostly up, a little off the
  // ground's slope.
  let angle = shape.x * 6.2831853;
  let facing = vec2f(cos(angle), sin(angle));
  let across = vec3f(-facing.y, facing.x, 0.0);
  let up = normalize(vec3f(0.0, 0.0, 1.0) + ground * 0.35);

  // How it bends (direction and radians, as one vector): its own lean...
  var bend = facing * (shape.y * shape.y * 0.6);
  // ...the wind: gusts rolling across the field the way it blows, and each
  // blade's own flutter, less for stiffer grass...
  let wind = frame.wind;
  let time = frame.time.x;
  let wave = sin(dot(root.xy, wind.xy) * 0.35 - time * 1.7) * 0.5 + 0.5;
  let gusts = mix(0.75, 0.3 + wave * 1.1, wind.w);
  let flutter = sin(time * (3.0 + shape.z * 2.5) + shape.z * 6.2831853) * 0.18;
  // Long grass waves; short grass hardly moves, mown grass not at all.
  let give = (1.0 - clamp(grass.blade.x, 0.0, 1.0) * 0.85) * smoothstep(0.03, 0.35, height);
  bend += wind.xy * (wind.z * give * 1.3 * (gusts + flutter));
  // ...and trampling, which wins where it's flat.
  let pressed = textureSampleLevel(trampleMap, trampleSampler,
                                   (root.xy - grass.trample.xy) * grass.trample.zw, 0.0);
  let pushed = pressed.rg * 2.0 - 1.0;
  if (pressed.b > 0.001 && dot(pushed, pushed) > 1e-6) {
    bend = mix(bend, normalize(pushed) * 1.4, pressed.b);
  }
  let amount = min(length(bend), 1.45);
  let toward = select(vec3f(facing, 0.0), vec3f(bend / max(length(bend), 1e-5), 0.0), amount > 1e-4);

  // The curve: rising from the root, bending over toward its tip.
  let tipDirection = normalize(up * cos(amount) + toward * sin(amount));
  let p1 = root + up * height * 0.55;
  let p2 = root + tipDirection * height;
  let a = 1.0 - t;
  let center = a * a * root + 2.0 * a * t * p1 + t * t * p2;
  let along = normalize(2.0 * a * (p1 - root) + 2.0 * t * (p2 - p1) + up * 1e-4);
  // Full width low down, narrowing to a point.
  let position = center + across * (side * width * 0.5 * (1.0 - t * t));

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
  out.grass = vec4f(height / max(grass.blade.w, 1e-4), t, shape.z, 0.0);
  out.grassData = textureSampleLevel(dataMap, trampleSampler, unpack2x16unorm(blade.look.w), 0.0);
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
  here.grass = vec4f(maps.g, 0.05, 0.5, 0.0);
  here.grassData = textureSampleLevel(dataMap, trampleSampler, uv, 0.0);
  let lit = shade(here);
  // As much as grass covers it, where it faces up enough to grow any.
  let coverage = maps.r;
  let up = smoothstep(0.45, 0.55, normalize(in.normal).z);
  let amount = coverage * up * grass.blade.z;
  return LitOut(vec4f(lit.color.rgb, amount), vec4f(lit.glow.rgb, amount));
}
