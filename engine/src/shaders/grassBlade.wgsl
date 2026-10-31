// A grass blade as it stands this frame, for the passes that draw blades
// (meshGrass.wgsl lit, meshGrassShadow.wgsl into the sun's shadow), so a
// blade's shadow leans, sways and flattens just as it does.
//
// The blade is a curve from its root that bends over toward the sum of its
// own lean, the wind and any trampling.

// A grown blade (see meshGrassGrow.wgsl).
struct GrownBlade {
  root: vec4f, // world root, height
  look: vec4u, // see meshGrassGrow.wgsl
}

// A grass field's settings for drawing it (GrassUniforms).
struct Grass {
  trample: vec4f, // xy the trample map's world min, zw 1 / its world size
  blade: vec4f,   // x stiffness (0..1), y draw distance, z how much the ground's tinted, w full height
  maps: vec4f,    // xy the maps' grid min (own space), zw 1 / its size (0: unpainted)
  shadow: vec4f,  // x self-shadow strength, y how far blades cast the sun's shadow (0: none),
                  // z how much wider their shadows are than they are
  model: mat4x4f, // the ground's own space to the world
  normalMatrix: mat4x4f,
  inverseModel: mat4x4f,
}

struct Blade {
  root: vec3f,
  height: f32,
  ground: vec3f, // its ground's normal
  width: f32,
  segments: u32,
  shape: vec4f,  // facing, lean, random, tone
  across: vec3f, // across its face
  up: vec3f,     // mostly up, a little off the ground's slope
  tip: vec3f,    // which way its tip points from its root
}

// A blade bent as it is now: by its own lean, the wind (the frame's) and
// `pressed` (the trample map at its root).
fn bladeAt(blade: GrownBlade, stiffness: f32, pressed: vec4f) -> Blade {
  var b: Blade;
  b.root = blade.root.xyz;
  b.height = blade.root.w;
  let tilt = unpack2x16snorm(blade.look.x);
  b.ground = vec3f(tilt, sqrt(max(1.0 - dot(tilt, tilt), 0.0)));
  let sizes = unpack2x16float(blade.look.y);
  b.width = sizes.x;
  b.segments = u32(sizes.y);
  b.shape = unpack4x8unorm(blade.look.z);

  // Which way it faces, and across it.
  let angle = b.shape.x * 6.2831853;
  let facing = vec2f(cos(angle), sin(angle));
  b.across = vec3f(-facing.y, facing.x, 0.0);
  b.up = normalize(vec3f(0.0, 0.0, 1.0) + b.ground * 0.35);

  // How it bends (direction and radians, as one vector): its own lean...
  var bend = facing * (b.shape.y * b.shape.y * 0.6);
  // ...the wind: gusts rolling across the field the way it blows, and each
  // blade's own flutter, less for stiffer grass...
  let wind = frame.wind;
  let time = frame.time.x;
  let wave = sin(dot(b.root.xy, wind.xy) * 0.35 - time * 1.7) * 0.5 + 0.5;
  let gusts = mix(0.75, 0.3 + wave * 1.1, wind.w);
  let flutter = sin(time * (3.0 + b.shape.z * 2.5) + b.shape.z * 6.2831853) * 0.18;
  // Long grass waves; short grass hardly moves, mown grass not at all.
  let give = (1.0 - clamp(stiffness, 0.0, 1.0) * 0.85) * smoothstep(0.03, 0.35, b.height);
  bend += wind.xy * (wind.z * give * 1.3 * (gusts + flutter));
  // ...and trampling, which wins where it's flat.
  let pushed = pressed.rg * 2.0 - 1.0;
  if (pressed.b > 0.001 && dot(pushed, pushed) > 1e-6) {
    bend = mix(bend, normalize(pushed) * 1.4, pressed.b);
  }
  let amount = min(length(bend), 1.45);
  let toward = select(vec3f(facing, 0.0), vec3f(bend / max(length(bend), 1e-5), 0.0), amount > 1e-4);
  b.tip = normalize(b.up * cos(amount) + toward * sin(amount));
  return b;
}

// The point `t` up a blade's curve (0 its root, 1 its tip): rising from the
// root, bending over toward its tip.
fn bladeCenter(b: Blade, t: f32) -> vec3f {
  let p1 = b.root + b.up * b.height * 0.55;
  let p2 = b.root + b.tip * b.height;
  let a = 1.0 - t;
  return a * a * b.root + 2.0 * a * t * p1 + t * t * p2;
}

// Which way the curve runs there (unit).
fn bladeAlong(b: Blade, t: f32) -> vec3f {
  let p1 = b.root + b.up * b.height * 0.55;
  let p2 = b.root + b.tip * b.height;
  return normalize(2.0 * (1.0 - t) * (p1 - b.root) + 2.0 * t * (p2 - p1) + b.up * 1e-4);
}
