// The lit pass's vertex stage for grass (after meshLit.wgsl, whose fragment
// stage lights it like any surface, with the field's material).
//
// Each instance is one blade, as meshGrassGrow.wgsl grew it this frame,
// drawn as a triangle strip up it: two vertices a row for its segments' rows
// from the root, then the tip, along its curve (grassBlade.wgsl), tapering
// to a point. Near where its detail drops, it takes on the shape it'll have
// with half the segments (its morph), so it doesn't change when it does.
//
// Before the blades, the ground they grow on is drawn again in their colour
// (vs_ground, fs_ground), as much as they cover it: so the gaps between
// blades look like grass. As the blades thin out to none it takes on how
// they look from afar, and carries on so out to the horizon.
//
// There are a great many blades, each a few pixels across but the nearest,
// so what each vertex hands on to its fragments costs as much as what they
// do with it: a vertex hands on as little as it can (BladeOut), and the
// fragments take what's the same all over the blade from the blade itself.
// What barely changes along a blade is found at its vertices, not at each of
// its points, where it pays (GRASS_AT_VERTICES): the rain always; the air
// and the clouds' shadow on the nearest blades, the clouds' shadow on the
// rest, and the sun's shadows on the far ones.

// GrownBlade, Grass and the blade's shape: grassBlade.wgsl (before this).

// What a pipeline's blades find at their vertices (shadeSeen()'s Found, see
// meshLit.wgsl), as bits: 1 what the air does (airAt()), 2 the clouds'
// shadow (cloudShadowAt()), 4 the sun's shadows (sunVisibility()) past
// grass.vertexSun.x, a blade at a time over the band of grass.vertexSun.y
// past it (by a number of its own, so they change over one by one, and stay
// changed wherever the camera goes; out there neither the blades' own
// shadows nor those of anything else change much along one). The nearest
// list's blades (many pixels each) find the air and the clouds there (3);
// farther ones (a few pixels each: as many points as vertices, so only
// what's dearest at a point) the clouds and the sun (6).
override GRASS_AT_VERTICES: u32 = 0u;

// How shut in a point `t` up a blade `height` tall is by the blades around
// it (the lit pass's SurfaceIn::grass.w): most at the root, none at the tip,
// and less in short grass, which has less over it. It takes the sky's light
// there and some of the sun's, so a field looks deep at any distance, past
// where its blades' own shadows are drawn.
fn deepIn(height: f32, t: f32) -> f32 {
  let below = 1.0 - t;
  return grass.shadow.x * below * sqrt(below) * mix(0.35, 1.0, smoothstep(0.03, 0.3, height));
}

// A blade's vertex, for its fragments: as little as can be (what's the same
// all over the blade they take from the blade, see bladeVertexOut()).
struct BladeOut {
  @builtin(position) clip: vec4f,
  @location(0) worldPosition: vec3f,
  @location(1) normal: vec3f,
  // x how far up it (0 root, 1 tip), y across it (its uv.x), z how shut in by
  // the blades around (deepIn()), w how wet (surfaceWetness())
  @location(2) along: vec4f,
  @location(3) dripped: f32, // dripLineWetness()
  @location(4) @interpolate(flat) blade: u32, // which, in `blades`
  // What it found at its vertices (GRASS_AT_VERTICES; sun below 0 where
  // it's found at each point).
  @location(5) air: vec4f,
  @location(6) clouds: f32,
  @location(7) sun: f32,
}

@group(2) @binding(0) var<storage, read> blades: array<GrownBlade>;
@group(2) @binding(1) var<uniform> grass: Grass;
@group(2) @binding(2) var trampleMap: texture_2d<f32>; // rg direction, b how flat
@group(2) @binding(3) var trampleSampler: sampler;
@group(2) @binding(4) var dataMap: texture_2d<f32>; // the maps' data channels
@group(2) @binding(5) var shapeMap: texture_2d<f32>; // r coverage, g length
// Counting the blades' work (SFS_GRASS_STATS): the fragments each pixel
// shades (fs_counted), a row grass.shadow.w long.
@group(2) @binding(6) var<storage, read_write> fragmentCounts: array<atomic<u32>>;
// Where the list drawn starts in `blades` (x).
@group(2) @binding(7) var<uniform> grassList: vec4u;
// The splat map of the terrain the grass grows on (its ground's material),
// for its ground past the maps (see fs_ground; a stand-in without one).
@group(2) @binding(8) var farSplat: texture_2d<f32>;

@vertex
fn vs_main(@builtin(vertex_index) index: u32, @builtin(instance_index) instance: u32) -> BladeOut {
  let grown = blades[grassList.x + instance];
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

  var center = bladeCenter(b, t);
  var along = bladeAlong(b, t);
  // Full width low down, narrowing to a point.
  var half = b.width * 0.5 * (1.0 - t * t);
  var deep = deepIn(height, t);
  // Toward the coarser blade's shape: on the straight line between the rows
  // either side that it has, as wide as it is there, and shaded as it is.
  if (b.morph > 0.0 && segments > 1u) {
    let coarse = f32(segments / 2u);
    let low = min(floor(t * coarse) / coarse, 1.0 - 1.0 / coarse);
    let high = low + 1.0 / coarse;
    let s = (t - low) * coarse;
    center = mix(center, mix(bladeCenter(b, low), bladeCenter(b, high), s), b.morph);
    along = normalize(mix(along, mix(bladeAlong(b, low), bladeAlong(b, high), s), b.morph));
    let narrows = mix(1.0 - low * low, 1.0 - high * high, s);
    half = mix(half, b.width * 0.5 * narrows, b.morph);
    deep = mix(deep, mix(deepIn(height, low), deepIn(height, high), s), b.morph);
  }
  let position = center + across * (side * half);

  // Its face, turned to the camera (blades show both sides), rounded across
  // its width and softened toward the ground's, so a field shades smoothly.
  var normal = normalize(cross(across, along));
  let turned = dot(normal, frame.cameraPosition.xyz - position) < 0.0;
  if (turned) {
    normal = -normal;
  }
  normal = normalize(normal + across * side * 0.3 + ground * 0.6);

  var out: BladeOut;
  out.clip = frame.viewProjection * vec4f(position, 1.0);
  out.worldPosition = position;
  out.normal = normal;
  // How wet it is in the rain, here rather than at each of its points.
  out.along = vec4f(t, side * 0.5 + 0.5, deep, surfaceWetness(position, normal));
  out.dripped = dripLineWetness(position, normal);
  out.blade = grassList.x + instance;
  if ((GRASS_AT_VERTICES & 1u) != 0u) {
    out.air = airAt(position);
  }
  if ((GRASS_AT_VERTICES & 2u) != 0u) {
    out.clouds = cloudShadowAt(position);
  }
  out.sun = -1.0;
  if ((GRASS_AT_VERTICES & 4u) != 0u) {
    let away = distance(b.root, frame.cameraPosition.xyz);
    if (away > grass.vertexSun.x + grass.vertexSun.y * fract(shape.z * 113.17 + shape.w * 37.31)) {
      // (Off the side its fragments light: the one the rasterizer finds
      // facing the camera, which isn't the one turned to it.)
      out.sun = sunVisibility(position, normal * select(1.0, -1.0, turned));
    }
  }
  return out;
}

// A blade's fragment, lit as any surface (a back face lit as seen, as fs_main
// does).
fn shadeBlade(in: BladeOut, front: bool) -> LitOut {
  var seen = bladeVertexOut(in);
  seen.normal = select(-seen.normal, seen.normal, front);
  let air = select(vec4f(-1.0), in.air, (GRASS_AT_VERTICES & 1u) != 0u);
  let clouds = select(-1.0, in.clouds, (GRASS_AT_VERTICES & 2u) != 0u);
  let sun = select(-1.0, in.sun, (GRASS_AT_VERTICES & 4u) != 0u);
  return shadeSeen(seen, Found(air, clouds, sun));
}

// A blade's point as the lit pass's fragment stage takes it (meshLit.wgsl):
// what's the same all over the blade from the blade itself.
fn bladeVertexOut(in: BladeOut) -> VertexOut {
  let grown = blades[in.blade];
  let shape = unpack4x8unorm(grown.look.z);
  let t = in.along.x;
  var out: VertexOut;
  out.clip = in.clip;
  out.worldPosition = in.worldPosition;
  out.normal = in.normal;
  // Lighter or darker, a little yellower or bluer: no two alike; and darker
  // down in the grass, where less light gets in.
  let light = 0.82 + shape.w * 0.3;
  let shift = (fract(shape.z * 7.31) - 0.5) * 0.16;
  out.vertexColor = vec3f(light * (1.0 + shift), light, light * (1.0 - shift)) * mix(0.55, 1.0, t);
  out.uv = vec2f(in.along.y, 1.0 - t);
  // (Its tile built lately, in the debug view (SFS_GRASS_TILES): blue, or
  // magenta for an edit; see bladeAt().)
  let stored = unpack2x16float(grown.look.y);
  out.tint = select(vec4f(1.0), select(vec4f(0.4, 1.2, 3.0, 1.0), vec4f(3.0, 0.4, 3.0, 1.0), stored.y < 0.0),
                    stored.x < 0.0);
  // For its material (the Grass node): its length, how far up it, its own
  // random number, and the maps' data where it grows.
  out.grass = vec4f(grown.root.w / max(grass.blade.w, 1e-4), t, shape.z, in.along.z);
  out.grassData = textureSampleLevel(dataMap, trampleSampler, unpack2x16unorm(grown.look.w), 0.0);
  out.rain = vec2f(in.along.w, in.dripped);
  return out;
}

// The ground under the grass, as its mesh (buffer 0 only): where it is,
// which way it faces, where it is on the mesh (its terrain's splat map lies
// over its UVs) and, where it's past the maps (a big mesh's window of them),
// what their data hold on average there, as the blades' material takes it.
struct GroundOut {
  @builtin(position) clip: vec4f,
  @location(0) worldPosition: vec3f,
  @location(1) normal: vec3f,
  @location(2) uv: vec2f,
  @location(3) data: vec4f,
}

// Where a point of the ground's own XY is on the maps, and how far in from
// their edge it is (all of them, unpainted, as they cover it all).
fn groundOnMaps(local: vec2f) -> vec3f {
  let uv = (local - grass.maps.xy) * grass.maps.zw;
  let edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
  return vec3f(uv, select(edge, 1.0, grass.maps.z <= 0.0));
}

@vertex
fn vs_ground(in: VertexIn) -> GroundOut {
  let world = (grass.model * vec4f(in.position, 1.0)).xyz;
  var out: GroundOut;
  out.clip = frame.viewProjection * vec4f(world, 1.0);
  out.worldPosition = world;
  out.normal = normalize((grass.normalMatrix * vec4f(in.normal, 0.0)).xyz);
  out.uv = in.uv;
  out.data = vec4f(0.0);
  if (groundOnMaps(in.position.xy).z < kGroundMapsEdge) {
    for (var y = 0; y < 4; y++) {
      for (var x = 0; x < 4; x++) {
        out.data += textureSampleLevel(dataMap, trampleSampler, (vec2f(f32(x), f32(y)) + 0.5) / 4.0, 0.0);
      }
    }
    out.data /= 16.0;
  }
  return out;
}

// Over how much of the maps' edge (a share of them) grass's ground hands
// over to what lies past them.
const kGroundMapsEdge = 0.04;

// Grass's ground as it stands in for blades seen from afar (fs_ground), as
// measured against them (by noon, golden hour, overcast and in autumn, within
// about 3%): its tone (the vertex colour's, as theirs is on average as seen
// from afar: mostly their upper parts), how far up a blade it's shut in by
// the others as (deepIn()), how much they lean back from facing the camera
// (bladesFacing()), how strong the grain of them is, the finest it gets (m),
// and how much lighter or darker it gets as gusts bend them.
const kFarTone = 0.37;
const kFarUp = 0.3;
const kFarLean = 1.67;
const kFarGrain = 1.3;
const kFarFinestGrain = 0.02;
const kFarGusts = 0.15;

// A hash of a lattice point, 0..1 (grass's ground's own).
fn groundHash(p: vec2f) -> f32 {
  let q = fract(p * vec2f(0.1031, 0.1030));
  let r = q + dot(q, q.yx + 33.33);
  return fract((r.x + r.y) * r.x);
}

// Smooth value noise, 0..1.
fn groundNoise(p: vec2f) -> f32 {
  let cell = floor(p);
  let f = p - cell;
  let u = f * f * (3.0 - 2.0 * f);
  let a = groundHash(cell);
  let b = groundHash(cell + vec2f(1.0, 0.0));
  let c = groundHash(cell + vec2f(0.0, 1.0));
  let d = groundHash(cell + vec2f(1.0, 1.0));
  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Noise in streaks along `along` (a unit direction over the ground), -0.5..0.5:
// cells `across` wide, kStreak times as long, fixed to the world, at the two
// sizes a power of 2 apart either side of it (blended, so it holds still as
// the camera moves).
fn groundStreaks(p: vec2f, along: vec2f, across: f32, seed: f32) -> f32 {
  let level = log2(across);
  let low = floor(level);
  let q = vec2f(dot(p, vec2f(-along.y, along.x)), dot(p, along) / kStreak);
  let a = groundNoise(q / exp2(low) + seed);
  let b = groundNoise(q / exp2(low + 1.0) + seed * 1.7);
  return mix(a, b, level - low) - 0.5;
}

// How much longer than wide a streak of grass's grain is on the ground: about
// a blade's height on screen, from afar.
const kStreak = 6.0;

// The grain of blades too far off to tell apart, -0.5..0.5: streaks fixed to
// the world, about two pixels (`pixel` m) across, running toward the camera
// (`toward`, over the ground), so they stand up on screen as blades do. (The
// world's, along four directions, the two nearest the camera's blended: so
// they don't turn as it moves.)
fn groundGrain(p: vec2f, toward: vec2f, pixel: f32, finest: f32) -> f32 {
  let across = max(pixel * 2.0, finest);
  let angle = atan2(toward.y, toward.x);
  let turn = fract(angle / 3.14159265) * 4.0;
  let k = floor(turn);
  let f = smoothstep(0.0, 1.0, turn - k);
  let a0 = k * 0.78539816;
  let a1 = a0 + 0.78539816;
  let first = groundStreaks(p, vec2f(cos(a0), sin(a0)), across, 17.0 + k * 13.0);
  let second = groundStreaks(p, vec2f(cos(a1), sin(a1)), across, 17.0 + ((k + 1.0) % 4.0) * 13.0);
  // (Two noises blended are flatter: as contrasty again.)
  return mix(first, second, f) * (1.0 + 0.8 * f * (1.0 - f));
}

// Which way the ground faces as it stands in for blades seen from afar,
// on `ground` (a unit normal): so the sun lights it as much as it does them
// on average. They face every way round, each turned toward the camera
// (`toward`, over the ground), and up off the ground by about `lean` (what's
// level for every 1 that's up); so the sun lights more of them the lower it
// is (their sides), and those it's behind more than those it's in front of.
// Its normal is then the one, between straight up and the sun's way over
// the ground, it lights as much.
fn bladesFacing(ground: vec3f, toward: vec3f, lean: f32) -> vec3f {
  let toSun = normalize(frame.sunDirection.xyz);
  let side = cross(ground, toward);
  var lit = 0.0;
  for (var i = 0; i < 6; i++) {
    let a = (f32(i) + 0.5) / 6.0 * 3.14159265 - 1.5707963;
    let facing = normalize(ground + (toward * cos(a) + side * sin(a)) * lean);
    lit += max(dot(facing, toSun), 0.0);
  }
  lit /= 6.0;
  let level = toSun - ground * dot(toSun, ground);
  if (dot(level, level) < 1e-6 || dot(toward, toward) < 0.5) {
    return ground;
  }
  let high = asin(clamp(dot(toSun, ground), -1.0, 1.0));
  let tilt = clamp(asin(clamp(lit, 0.0, 1.0)) - high, -1.2, 1.2);
  return normalize(ground * cos(tilt) + normalize(level) * sin(tilt));
}

@fragment
fn fs_ground(in: GroundOut) -> LitOut {
  // How many metres a pixel spans here (in uniform control flow).
  let pixel = min(length(dpdx(in.worldPosition)), length(dpdy(in.worldPosition)));
  // Its maps here: how much grass covers it, and (for the material, as the
  // blades see them) how long it is and its data.
  let local = (grass.inverseModel * vec4f(in.worldPosition, 1.0)).xy;
  let onMaps = groundOnMaps(local);
  let maps = textureSampleLevel(shapeMap, trampleSampler, onMaps.xy, 0.0);
  var coverage = maps.r;
  var tall = maps.g;
  var data = textureSampleLevel(dataMap, trampleSampler, onMaps.xy, 0.0);
  // Past them, grass where its terrain's layers are meadow, long, with the
  // maps' data as they hold it on average; handing over at their edge.
  let inside = smoothstep(0.0, kGroundMapsEdge, onMaps.z);
  if (inside < 1.0) {
    var meadow = 0.0;
    if (grass.far.y > 0.5) {
      let splat = clamp(textureSampleLevel(farSplat, trampleSampler, in.uv, 0.0), vec4f(0.0), vec4f(1.0));
      let sum = splat.x + splat.y + splat.z + splat.w;
      let shares = select(splat, splat / sum, sum > 1.0);
      meadow = max(1.0 - min(sum, 1.0), 0.0) * grass.far.x + dot(shares, grass.farMeadow);
    }
    coverage = mix(meadow, coverage, inside);
    tall = mix(1.0, tall, inside);
    data = mix(in.data, data, inside);
  }

  let normal = normalize(in.normal);
  let toCamera = frame.cameraPosition.xyz - in.worldPosition;
  let distanceAway = length(toCamera);
  let away = distanceAway / max(grass.blade.y, 1.0);
  // Near, the ground down between the blades, in among them: shaded by the
  // grass around it, and a field seen from afar shows more of their tops,
  // so a little less; farther out, where it stands in for the blades
  // thinning out, as dark as they look together there (their lower parts,
  // and the shade between them, show among their tops).
  var tone = vec3f(0.62 * mix(1.0, 0.84, smoothstep(0.3, 0.9, away) * coverage));
  var deep = deepIn(tall * grass.blade.w, mix(0.15, 0.45, smoothstep(0.2, 1.0, away)));
  var facing = normal;
  // As the blades thin out to none (from grass.far.z to the draw distance),
  // and past them, it looks as they do from afar instead: as bright as they
  // are on average, turned toward the camera as they are, a grain of them
  // too fine to tell apart, and their gusts rolling over it.
  let gone = smoothstep(grass.far.z, grass.blade.y, distanceAway);
  if (gone > 0.0) {
    let level = toCamera - normal * dot(toCamera, normal);
    let toward = select(vec3f(0.0), normalize(level), dot(level, level) > 1e-6);
    let bladeFacing = bladesFacing(normal, toward, kFarLean);
    let wind = frame.wind;
    let wave = sin(dot(in.worldPosition.xy, wind.xy) * 0.35 - frame.time.x * 1.7) * 0.5 + 0.5;
    let gusts = mix(0.75, 0.3 + wave * 1.1, wind.w);
    let give = (1.0 - clamp(grass.blade.x, 0.0, 1.0) * 0.85) * smoothstep(0.03, 0.35, tall * grass.blade.w);
    let swayed = 1.0 + (gusts - 0.75) * clamp(wind.z, 0.0, 1.5) * give * kFarGusts;
    let grain = groundGrain(in.worldPosition.xy, toward.xy, pixel, kFarFinestGrain);
    let farTone = kFarTone * (1.0 + grain * kFarGrain) * swayed;
    tone = mix(tone, vec3f(farTone), gone);
    deep = mix(deep, deepIn(tall * grass.blade.w, kFarUp), gone);
    facing = normalize(mix(normal, bladeFacing, gone));
  }

  var here: VertexOut;
  here.clip = in.clip;
  here.worldPosition = in.worldPosition;
  here.normal = facing;
  here.vertexColor = tone;
  here.uv = vec2f(0.5, 0.95);
  here.tint = vec4f(1.0);
  here.grass = vec4f(tall, 0.05, 0.5, deep);
  here.grassData = data;
  here.rain = vec2f(-1.0, 0.0);
  let lit = shade(here);
  // As much as grass covers it, where it faces up enough to grow any (out
  // to the horizon: past the maps, as much as its terrain's layers are
  // meadow, see meshTerrainSurface.wgsl); under the blades a little of the
  // ground's own material shows through, but none once they're gone (it'd
  // keep its own colour, not the grass's, through the seasons).
  let up = smoothstep(0.45, 0.55, normal.z);
  let amount = coverage * up * mix(grass.blade.z, 1.0, gone);
  return LitOut(vec4f(lit.color.rgb, amount), vec4f(lit.glow.rgb, amount));
}

// The blades' fragments (shadeBlade(): see meshGrassVertexLit.wgsl).
@fragment
fn fs_blade(in: BladeOut, @builtin(front_facing) front: bool) -> LitOut {
  return shadeBlade(in, front);
}

// The same, counted at their pixels.
@fragment
fn fs_counted(in: BladeOut, @builtin(front_facing) front: bool) -> LitOut {
  let pixel = vec2u(in.clip.xy);
  atomicAdd(&fragmentCounts[pixel.y * u32(grass.shadow.w) + pixel.x], 1u);
  return shadeBlade(in, front);
}
