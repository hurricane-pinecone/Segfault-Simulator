// Grows a grass field's blades for a frame (a compute pass before the lit
// pass), from its ground and maps: only those in view, nearest first in
// detail, into three lists (a blade's detail: 4, 2 or 1 segments) that the lit
// pass draws indirectly (meshGrass.wgsl). Those near enough to cast the sun's
// shadow are listed again (by index) for the shadow pass
// (meshGrassShadow.wgsl).
//
// Blades are spread over the ground's own XY on four lattices, each twice as
// coarse as the one before. Each lattice cell has one blade, at a random
// place in it (from a hash of the cell, so it's there every frame). The
// lattices start at the ground's own origin, not its grid's corner, so a
// blade stays put when a big mesh's window of ground moves (see
// GrassModule), and two fields on the same mesh space line up. A cell whose
// coordinates are both even is left to the next lattice's, so they add up to
// one blade per finest cell.
//
// Farther away, fewer are kept: the share kept falls smoothly with distance
// (grassKept), to none at the draw distance. Each blade has its own fixed
// rank (from its cell's hash: the coarser its lattice, the lower), and is
// kept where its rank is below that share, so it's the same blades wherever
// the camera is, and a patch thins out evenly, blade by blade, as the camera
// draws away. A blade near its cut-off grows in over a short stretch rather
// than popping, and those left are wider (by how many are), so the field
// reads as full at any distance. (With thinning.w 0, the old way: each
// lattice reaching so far from the camera, shrinking away at its end.)
//
// A blade's detail falls in steps, but over the stretch before each step it
// takes on the coarser shape (morph, see meshGrass.wgsl), so a step doesn't
// show.
//
// Blades are kept from frame to frame, in tiles (GrassTileCache): cs_build
// works out a tile's blades once, all that doesn't depend on the camera
// (where each is on the ground, how tall the maps let it grow, its ground's
// slope), and keeps those ranked low enough to be drawn near it; cs_grow
// then grows a frame's blades from the tiles in view, doing only what does
// (how many are kept this far away, in view, detail). cs_main grows them all
// from the ground and maps each frame (the old way, and for comparing).
//
// Grown from tiles, the blades are listed about front to back (see
// cs_count), so the lit pass's depth test hides more of the farther ones
// behind those drawn before them, rather than shading them.

struct Grow {
  model: mat4x4f,         // the ground's own space to the world
  normalMatrix: mat4x4f,  // its normals to the world's (upper 3 x 3)
  planes: array<vec4f, 6>, // the view frustum's, inward (dot(xyz, p) + w >= 0)
  camera: vec4f,          // xyz the camera, w draw distance
  ground: vec4f,          // xy the ground grid's min, zw its texel size
  groundSize: vec4i,      // xy texels
  maps: vec4f,            // xy the maps' grid min, zw 1 / its size (0: unpainted)
  shape: vec4f,           // height, height variation, width, finest cell
  reach: vec4f,           // each lattice's reach (world units)
  detail: vec4f,          // x 4 segments to here, y 2 to here, z casting shadows
                          // to here (world units; 0: none), w counting (1)
  thinning: vec4f,        // x all kept to here, y how fast fewer are kept past it
                          // (a power), z fewer still from here (none at the draw
                          // distance), w 1 (0: the old way, by lattice)
  capacity: vec4u,        // each list's room (blades), and w the shadow casters'
  lattices: array<vec4i, 4>, // each one's first cell (xy) and how many (zw)
  tiles: vec4u,           // the tiles to build, where those grown from start, how
                          // many of them, and their chunks (of kChunk blades)
  // Tiles hidden behind what's drawn opaque left out (see hiddenTile()): the
  // view, the camera (xyz; not grass's own), which way it looks (xyz) and how
  // far a blade's shadow falls from it, for its height (w; below 0: too far
  // to tell), and `farthest`'s finest level's size (xy texels) and levels (z;
  // 0: none).
  viewProjection: mat4x4f,
  eye: vec4f,
  forward: vec4f,
  hidden: vec4f,
  // Its vigour (GrassVigourSettings, see grassVigourUniforms()): x scale, y
  // bias, z the least length, w the least density; the field's world min and
  // 1 / its size (z 0: none); x how much detail noise, y its size, z 1 if it
  // drives Data 0, w the field past it.
  vigour: vec4f,
  vigourRect: vec4f,
  vigourDetail: vec4f,
}

// A tile to build, or to grow blades from (GrassTileEntry).
struct TileEntry {
  origin: vec2i,   // its first cell, on its lattice
  lattice: u32,    // which, and 4 built lately, 8 built for an edit (debug
                   // view), 16 grown from its high Y first
  slot: u32,       // where its count is
  first: u32,      // its first kept blade
  room: u32,       // how many it has room for
  threshold: f32,  // built: those ranked below this are kept
  firstChunk: u32, // grown from: its first chunk of the frame's
  heights: vec2f,  // grown from: its ground's lowest and highest (its own Z)
  unused: vec2f,
}

// A tile's side, in its lattice's cells; and blades a chunk (cs_grow's
// workgroup).
const kTileCells = 64u;
const kChunk = 64u;

// Over how much of its rank a blade grows in (as a share of it): those
// ranked up to kept / (1 + this) are full grown.
const kGrowIn = 0.2;
// The most wider blades are where few are kept, and how fast they widen.
const kMostWider = 4.0;
const kWiden = 0.5;
// Over how much of the distance to a step in detail a blade takes on the
// coarser shape.
const kMorph = 0.3;

// The share of blades kept at a distance (before growing in): all near the
// camera, then fewer by a power of the distance, and none at the draw
// distance.
fn grassKept(away: f32) -> f32 {
  return grassFull(away) * (1.0 - smoothstep(grow.thinning.z, grow.camera.w, away));
}

// The share kept, but for fading out toward the draw distance.
fn grassFull(away: f32) -> f32 {
  return min(pow(grow.thinning.x / max(away, 1e-3), grow.thinning.y), 1.0);
}

// A grown blade, 32 bytes:
//   root: xyz its world root, w its height
//   look.x: its ground's world normal, xy (snorm16 x 2; z up, from them)
//   look.y: width, and segments plus how far it's taken on the coarser
//           shape (0..1) (float16 x 2)
//   look.z: facing (of a turn), lean, random, tone (unorm8 x 4)
//   look.w: the maps' data where it grows (unorm8 x 4)
struct GrownBlade {
  root: vec4f,
  look: vec4u,
}

@group(0) @binding(0) var<uniform> grow: Grow;
@group(0) @binding(1) var groundMap: texture_2d<f32>; // xyz normal, w height
@group(0) @binding(2) var shapeMap: texture_2d<f32>;  // r coverage, g length
@group(0) @binding(3) var mapSampler: sampler;
@group(0) @binding(4) var<storage, read_write> blades: array<GrownBlade>;
// Four indirect draws (vertex count, instance count, first vertex, first
// instance): each list's, then the shadow casters'. Then where each list
// starts in `blades`, at [64], [128] and [192] (for the lit pass, which
// reads one at a time, 256 bytes apart).
@group(0) @binding(5) var<storage, read_write> draws: array<atomic<u32>>;
// The shadow casters: their blades' indices.
@group(0) @binding(6) var<storage, read_write> casters: array<u32>;
// Counting its work (SFS_GRASS_STATS; detail.w 1), at each step: see
// count().
@group(0) @binding(7) var<storage, read_write> counters: array<atomic<u32>, 20>;
// Kept in tiles: the tiles (those to build, then those to grow from), their
// blades (KeptBlade) and, by slot, how many each has (x) and this frame's
// blades drawn from it in each list (yzw: counted, then where they go).
@group(0) @binding(8) var<storage, read> tiles: array<TileEntry>;
@group(0) @binding(9) var<storage, read_write> cache: array<vec4u>;
@group(0) @binding(10) var<storage, read_write> tileState: array<vec4u>;
// This frame, for each of its chunks' blades (TileEntry::firstChunk): which
// list it's drawn in (1-3), or 0 if it isn't.
@group(0) @binding(11) var<storage, read_write> listedIn: array<u32>;
// The farthest depth along the view of what's drawn opaque over each block
// of the screen (nothing: very far; grassCulling.wgsl: as its bits), its
// levels each with blocks twice the size.
@group(0) @binding(12) var farthest: texture_2d<u32>;
// The maps as they are now, growing (GrassMaps::nowPixels(): r coverage, g
// length, b the band, a bounds), where they've been cut (cutPixels(): r the
// cut's edge, g and b the way the blades lean, a how long the cut left them),
// their data, and the field grass grows by (vigour).
@group(0) @binding(13) var nowMap: texture_2d<f32>;
@group(0) @binding(14) var cutMap: texture_2d<f32>;
@group(0) @binding(15) var dataMap: texture_2d<f32>;
@group(0) @binding(16) var vigourMap: texture_2d<f32>;

// One more blade at a step: 0 cells (or kept blades) looked at, 1 on ground
// and kept this far away, 2 covered (by its maps), 3 tall enough to draw, 4
// in view, 5-7 listed in each list, 8 casting shadows, 9 out of room, 10-13
// listed from each lattice; building tiles, 14 cells looked at, 15 kept, 16
// out of the tile's room; 17 tiles hidden.
fn count(step: u32) {
  if (grow.detail.w > 0.0) {
    atomicAdd(&counters[step], 1u);
  }
}

fn pcg(value: u32) -> u32 {
  let state = value * 747796405u + 2891336453u;
  let word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}

// The next of a cell's random numbers, 0 (inclusive) to 1 (exclusive).
fn random(seed: ptr<function, u32>) -> f32 {
  *seed = pcg(*seed);
  return f32(*seed >> 8u) / 16777216.0;
}

fn hasGround(texel: vec4f) -> bool {
  return any(texel.xyz != vec3f(0.0));
}

// The ground's height at a point (texels), between the texels around it that
// have ground.
fn heightAt(at: vec2f, fallback: f32) -> f32 {
  let p = at - 0.5;
  let low = vec2i(floor(p));
  let t = p - floor(p);
  var total = 0.0;
  var weight = 0.0;
  for (var k = 0; k < 4; k++) {
    let offset = vec2i(k & 1, k >> 1);
    let texel = low + offset;
    if (any(texel < vec2i(0)) || any(texel >= grow.groundSize.xy)) {
      continue;
    }
    let g = textureLoad(groundMap, texel, 0);
    if (!hasGround(g)) {
      continue;
    }
    let w = select(1.0 - t.x, t.x, offset.x == 1) * select(1.0 - t.y, t.y, offset.y == 1);
    total += g.w * w;
    weight += w;
  }
  return select(fallback, total / weight, weight > 1e-5);
}

// A hash of a lattice point, 0..1, and smooth value noise from it (the
// maps' own, for grassShape()).
fn shapeHash(p: vec2f) -> f32 {
  let q = fract(p * vec2f(0.1031, 0.1030));
  let r = q + dot(q, q.yx + 33.33);
  return fract((r.x + r.y) * r.x);
}

fn shapeNoise(p: vec2f) -> f32 {
  let cell = floor(p);
  let f = p - cell;
  let u = f * f * (3.0 - 2.0 * f);
  let a = shapeHash(cell);
  let b = shapeHash(cell + vec2f(1.0, 0.0));
  let c = shapeHash(cell + vec2f(0.0, 1.0));
  let d = shapeHash(cell + vec2f(1.0, 1.0));
  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// The shape maps at `uv` as grass grows to them: r how much grows, g how
// long, and b where it may grow at all (their bounds, read between texels:
// it grows where they're at least a half, to the blade, see
// GrassMaps::boundsValue()). Where coverage falls (worn or worked ground) it
// gives out over a band (the maps' b, grassEdgeBand(): how far in from the
// fall a point is), thinner and shorter toward it, and where it's part
// covered, in clumps with gaps between (a worn way's surviving tufts) rather
// than evenly sparse; the clumps' noise is fixed to the maps (so to the
// ground: it holds still as the camera moves) and wanders a clump's edge
// across the band, so the maps' texels never show as squares nor an edge as
// a straight line. Deep in full grass (most of it) none of that's needed.
// meshGrass.wgsl's (the ground under the blades) takes their density on
// average instead of the clumps: keep its band as this.
fn grassShape(uv: vec2f) -> vec4f {
  return grassShapeOf(textureSampleLevel(shapeMap, mapSampler, uv, 0.0), uv);
}

// The same, of maps as read at `uv`.
fn grassShapeOf(maps: vec4f, uv: vec2f) -> vec4f {
  if (min(maps.r, maps.b) >= 0.999) {
    return vec4f(1.0, maps.g, maps.a, 0.0);
  }
  let t = uv * vec2f(textureDimensions(shapeMap));
  // (The band's against the grass here: part-covered ground as a whole is
  // in clumps, not given out.)
  let edge = smoothstep(0.25, 1.0, maps.b / max(maps.r, 1e-3));
  let noise = shapeNoise(t * 0.8 + 71.0) * 0.6 + shapeNoise(t * 0.3 + 3.1) * 0.4;
  let clump = mix(0.1, 0.9, noise);
  // (None at all where nothing's covered: bare is bare, the clumps' noise
  // never lifts it.)
  let cover = clamp((maps.r * edge - clump) / 0.5 + 0.5, 0.0, 1.0) * step(0.002, maps.r);
  return vec4f(cover, maps.g * mix(0.45, 1.0, edge), maps.a, 0.0);
}

// The maps as they are now at `uv` (growing): x coverage, y length. Where
// they've been cut, a blade takes its length only from the texels on its own
// side of the cut's edge (the cut map read between them and cut at a half,
// as bounds are), so the edge is where the mower's was, to the blade.
fn nowAt(uv: vec2f) -> vec2f {
  let size = vec2i(textureDimensions(nowMap));
  let p = uv * vec2f(size) - 0.5;
  let low = vec2i(floor(p));
  let f = p - floor(p);
  var cover = 0.0;
  var edge = 0.0;
  var lengths: array<f32, 4>;
  var cuts: array<f32, 4>;
  var weights: array<f32, 4>;
  for (var k = 0; k < 4; k++) {
    let offset = vec2i(k & 1, k >> 1);
    let texel = clamp(low + offset, vec2i(0), size - 1);
    let now = textureLoad(nowMap, texel, 0);
    let cut = textureLoad(cutMap, texel, 0).r;
    let w = select(1.0 - f.x, f.x, offset.x == 1) * select(1.0 - f.y, f.y, offset.y == 1);
    cover += now.r * w;
    edge += cut * w;
    lengths[k] = now.g;
    cuts[k] = cut;
    weights[k] = w;
  }
  let inside = edge >= 0.5;
  var all = 0.0;
  var side = 0.0;
  var sum = 0.0;
  for (var k = 0; k < 4; k++) {
    all += lengths[k] * weights[k];
    if ((cuts[k] >= 0.5) == inside) {
      side += lengths[k] * weights[k];
      sum += weights[k];
    }
  }
  return vec2f(cover, select(all, side / max(sum, 1e-4), sum > 1e-4));
}

// How mown grass lies at `uv` (cutPixels(), read between texels): xy the way
// its blades lean (by how much: a roller's 1), z how long the cut left it, w
// whether it's cut (at least a half).
fn laidAt(uv: vec2f) -> vec4f {
  let size = vec2i(textureDimensions(cutMap));
  let p = uv * vec2f(size) - 0.5;
  let low = vec2i(floor(p));
  let f = p - floor(p);
  var sum = vec4f(0.0);
  for (var k = 0; k < 4; k++) {
    let offset = vec2i(k & 1, k >> 1);
    let texel = clamp(low + offset, vec2i(0), size - 1);
    let w = select(1.0 - f.x, f.x, offset.x == 1) * select(1.0 - f.y, f.y, offset.y == 1);
    sum += textureLoad(cutMap, texel, 0) * w;
  }
  return vec4f(sum.gb * (255.0 / 127.5) - 1.0, sum.a, sum.r);
}

// A blade's height over mown grass: cut level (a little either way), not as
// tall or short as it grows of itself, till it's grown back past the cut.
fn mownHeight(full: f32, length: f32, tall: f32, laid: vec4f) -> f32 {
  if (laid.w < 0.5) {
    return full;
  }
  let level = grow.shape.x * length * (1.0 + (tall - 0.5) * 0.08);
  return mix(level, full, smoothstep(0.0, 0.3, length - laid.z));
}

// Short blades (mown) wider, a stubbier, fuller tuft: as much ground covered by as many blades.
fn stubby(height: f32) -> f32 {
  return mix(1.8, 1.0, smoothstep(0.12, 0.45, height / max(grow.shape.x, 1e-3)));
}

// How the maps grow a blade as they are now at `uv`: grassShape()'s, from
// what they grow to (`shapeMap`'s band and bounds, and its length times
// `most`, the blade's vigour's) and how they are now.
fn grassShapeNow(uv: vec2f, most: f32) -> vec4f {
  let goal = textureSampleLevel(shapeMap, mapSampler, uv, 0.0);
  let now = nowAt(uv);
  return grassShapeOf(vec4f(now.x, min(now.y, goal.g * most), goal.b, goal.a), uv);
}

// Vigour (GrassVigourSettings): the field at a world point (`outside` past
// it, handing over at its edge)...
fn vigourField(world: vec2f) -> f32 {
  if (grow.vigourRect.z <= 0.0) {
    return grow.vigourDetail.w;
  }
  let uv = (world - grow.vigourRect.xy) * grow.vigourRect.zw;
  let edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
  if (edge <= 0.0) {
    return grow.vigourDetail.w;
  }
  let field = textureSampleLevel(vigourMap, mapSampler, uv, 0.0).r;
  return mix(grow.vigourDetail.w, field, smoothstep(0.0, 0.03, edge));
}

// ...the ground's smooth detail noise over it (0..1, grassVigourDetail())...
fn vigourDetailAt(world: vec2f) -> f32 {
  let size = grow.vigourDetail.y;
  return shapeNoise(world / size + 17.3) * 0.65 + shapeNoise(world / (size * 0.37) - 5.1) * 0.35;
}

// ...and the vigour of both (`detail` that noise's).
fn vigourOf(world: vec2f, detail: f32) -> f32 {
  if (grow.vigour.x == 0.0) {
    return smoothstep(0.0, 1.0, grow.vigour.y); // (off: 1)
  }
  let w = vigourField(world) + (detail - 0.5) * grow.vigourDetail.x;
  return smoothstep(0.0, 1.0, w * grow.vigour.x + grow.vigour.y);
}

// Whether a blade's kept by its vigour (fewer where it's low), by a random
// number of its own (from its cell's seed).
fn vigourKeeps(seed: u32, vigour: f32) -> bool {
  let thin = f32(pcg(seed ^ 0x2c1b3c6du) >> 8u) / 16777216.0;
  return thin < mix(grow.vigour.w, 1.0, vigour);
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) id: vec3u) {
  let lattice = id.z;
  let span = grow.lattices[lattice];
  if (i32(id.x) >= span.z || i32(id.y) >= span.w) {
    return;
  }
  let cell = span.xy + vec2i(id.xy);
  if (lattice < 3u && (cell.x & 1) == 0 && (cell.y & 1) == 0) {
    return; // the next lattice's
  }
  count(0u);
  var seed = pcg(bitcast<u32>(cell.x) ^ pcg(bitcast<u32>(cell.y) ^ pcg(lattice + 40503u)));
  let size = grow.shape.w * f32(1u << lattice);
  let local = (vec2f(cell) + vec2f(random(&seed), random(&seed))) * size;

  // The ground under it.
  let at = (local - grow.ground.xy) / grow.ground.zw;
  let texel = vec2i(floor(at));
  if (any(texel < vec2i(0)) || any(texel >= grow.groundSize.xy)) {
    return;
  }
  let ground = textureLoad(groundMap, texel, 0);
  if (!hasGround(ground)) {
    return;
  }
  // How far it is, from its texel's height (near enough, and cheaper than
  // its own: most are dropped by it).
  let away = distance((grow.model * vec4f(local, ground.w, 1.0)).xyz, grow.camera.xyz);
  let reach = grow.reach[lattice];
  if (away > reach) {
    return;
  }

  // Kept, this far away? By its rank: the finest lattice's blades (3/4 of
  // the finest cells) rank in the top 3/4, the next's in the 3/16 below...
  // and the coarsest's lowest, so a lattice's blades go before the coarser
  // ones', and a blade's always the same one.
  let stable = grow.thinning.w > 0.5;
  var grown = 1.0;
  if (stable) {
    let top = select(1.0 / 64.0, 1.0 / f32(1u << (2u * lattice)), lattice < 3u);
    let bottom = select(0.0, top * 0.25, lattice < 3u);
    let rank = mix(bottom, top, f32(pcg(seed ^ 0x68bc21ebu) >> 8u) / 16777216.0);
    let kept = grassKept(away) * (1.0 + kGrowIn);
    if (rank >= kept) {
      return;
    }
    grown = smoothstep(0.0, 1.0, log2(kept / max(rank, 1e-9)) / log2(1.0 + kGrowIn));
  }
  count(1u);
  let root = (grow.model * vec4f(local, heightAt(at, ground.w), 1.0)).xyz;

  // As its maps say (as they are now): whether it grows here, and how long;
  // and its vigour.
  let vigour = vigourOf(root.xy, vigourDetailAt(root.xy));
  let uv = (local - grow.maps.xy) * grow.maps.zw;
  let maps = grassShapeNow(uv, mix(grow.vigour.z, 1.0, vigour));
  let keep = random(&seed);
  let tall = random(&seed);
  if (keep >= maps.r * step(0.5, maps.b) || !vigourKeeps(seed, vigour)) {
    return;
  }
  count(2u);
  let laid = laidAt(uv);
  var height = mownHeight(grow.shape.x * max(1.0 + grow.shape.y * (tall * 2.0 - 1.0) * 0.5, 0.05) * maps.g,
                          maps.g, tall, laid);
  var width = grow.shape.z * stubby(height);
  if (stable) {
    // Growing in; and wider where fewer are kept, so the field still reads
    // full.
    height *= grown;
    width *= min(pow(grassFull(away), -kWiden), kMostWider) * mix(0.4, 1.0, grown);
  } else {
    // Shrinking away; the last lattice's over a longer stretch, down into
    // the ground at the draw distance rather than ending at a line. Wider by
    // how many blades are left of those near the camera: each lattice's
    // share (as it shrinks away) of the finest cells.
    height *= 1.0 - smoothstep(reach * select(0.7, 0.55, lattice == 3u), reach, away);
    var left = 0.0;
    for (var k = 0; k < 4; k++) {
      let share = select(0.75 / f32(1u << u32(2 * k)), 1.0 / 64.0, k == 3);
      left += share * (1.0 - smoothstep(grow.reach[k] * 0.7, grow.reach[k], away));
    }
    width *= min(pow(max(left, 1.0 / 64.0), -0.4), 4.0);
  }
  let normal = normalize((grow.normalMatrix * vec4f(ground.xyz, 0.0)).xyz);
  place(root, height, width, away, pack2x16snorm(normal.xy), uv, lattice, seed, 0u, vigour, laid.xy);
}

// A blade grown this frame (`seed` its cell's, past its place, rank and
// maps' randoms): if tall enough and in view, listed in its detail's list
// (and as a shadow caster, if near enough). `marks`: TileEntry::lattice's 4
// and 8 (debug view), shown by its width's and segments' signs.
fn place(root: vec3f, height: f32, width: f32, away: f32, normal: u32, uv: vec2f,
         lattice: u32, seedAfter: u32, marks: u32, vigour: f32, lean: vec2f) {
  if (!shown(root, height)) {
    return;
  }
  // Short grass near the camera goes in a coarser list while it has room,
  // else in its distance's.
  let detail = detailOf(away, height);
  let far = min(u32(detail.x), 2u);
  var level = detail.y;
  var list = min(u32(level), 2u);
  var index = atomicAdd(&draws[list * 4u + 1u], 1u);
  if (index >= grow.capacity[list] && list != far) {
    atomicSub(&draws[list * 4u + 1u], 1u);
    list = far;
    level = detail.x;
    index = atomicAdd(&draws[list * 4u + 1u], 1u);
  }
  if (index >= grow.capacity[list]) {
    atomicSub(&draws[list * 4u + 1u], 1u); // out of room: one fewer
    count(9u);
    return;
  }
  var first = 0u;
  for (var l = 0u; l < list; l++) {
    first += grow.capacity[l];
  }
  put(first + index, list, level, root, height, width, away, normal, uv, lattice, seedAfter, marks,
      vigour, lean);
}

// Whether a blade's tall enough to draw, and in view: a sphere round it,
// with room to lean.
fn shown(root: vec3f, height: f32) -> bool {
  if (height < 0.004) {
    return false;
  }
  count(3u);
  let middle = root + vec3f(0.0, 0.0, height * 0.5);
  for (var p = 0; p < 6; p++) {
    if (dot(grow.planes[p].xyz, middle) + grow.planes[p].w < -height * 1.2) {
      return false;
    }
  }
  count(4u);
  return true;
}

// A blade's detail, as a level from 0 (4 segments) to 2 (1): x by its
// distance alone, y coarser for short grass too (mown grass hardly curves),
// each step taken over a stretch (morphing to the coarser shape) rather than
// at once.
fn detailOf(away: f32, height: f32) -> vec2f {
  let byDistance = smoothstep(grow.detail.x * (1.0 - kMorph), grow.detail.x, away) +
                   smoothstep(grow.detail.y * (1.0 - kMorph), grow.detail.y, away);
  let byLength = 2.0 - smoothstep(0.18, 0.24, height) - smoothstep(0.08, 0.11, height);
  return vec2f(byDistance, min(max(byDistance, byLength), 2.0));
}

// A blade listed, at `at` of `blades`, in `list` (its detail `level`).
fn put(at: u32, list: u32, level: f32, root: vec3f, height: f32, width: f32, away: f32,
       normal: u32, uv: vec2f, lattice: u32, seedAfter: u32, marks: u32, vigour: f32,
       laid: vec2f) {
  let segments = select(select(1.0, 2.0, list == 1u), 4.0, list == 0u);
  let morph = select(min(level - f32(list), 0.99), 0.0, list == 2u); // (float16: below 1)

  var seed = seedAfter;
  var facing = random(&seed);
  // (Its own lean: grassBlade.wgsl bends it lean^2 x 1.2 radians, up to 0.6.)
  var lean = random(&seed) * 0.7071;
  let blade = random(&seed);
  let tone = random(&seed);
  // Mown grass laid over (a roller's stripes): facing the way it was rolled,
  // give or take, and leaning over that way by how hard (to about 65 degrees,
  // where rolled grass lies).
  let pressed = length(laid);
  if (pressed > 0.02) {
    let toward = atan2(laid.y, laid.x) / 6.2831853 + (facing - 0.5) * 0.14;
    let own = facing;
    facing = fract(mix(own, toward, smoothstep(0.02, 0.3, pressed)) + 1.0);
    lean = max(lean, sqrt(min(pressed, 1.0) * 0.95));
  }
  let sizes = vec2f(width, segments + morph) *
              select(vec2f(1.0), vec2f(-1.0), vec2<bool>((marks & 4u) != 0u, (marks & 8u) != 0u));
  // Its maps' data where it grows, for its material (Data 0 by its vigour,
  // if that drives it).
  var data = textureSampleLevel(dataMap, mapSampler, clamp(uv, vec2f(0.0), vec2f(1.0)), 0.0);
  if (grow.vigourDetail.z > 0.5) {
    data.x = 1.0 - vigour;
  }
  blades[at] = GrownBlade(
      vec4f(root, height),
      vec4u(normal,
            pack2x16float(sizes),
            pack4x8unorm(vec4f(facing, lean, blade, tone)),
            pack4x8unorm(data)));
  count(5u + list);
  count(10u + lattice);

  // Near enough that its shadow shows: listed for the shadow pass too.
  if (away < grow.detail.z) {
    let caster = atomicAdd(&draws[13], 1u);
    if (caster < grow.capacity.w) {
      casters[caster] = at;
      count(8u);
    } else {
      atomicSub(&draws[13], 1u);
    }
  }
}

// --- Kept in tiles -----------------------------------------------------------

// A tile's blade, kept (16 bytes):
//   x: its cell in the tile (6 bits x, 6 y), then its height (unorm 12 bits,
//      of the tallest a blade grows, as its maps grow it to), then the
//      ground's detail noise there (unorm 8 bits, vigourDetailAt())
//   y: its root's height, z its ground texel's (the ground's own Z; f32)
//   w: its ground's world normal, xy (snorm16 x 2)

// The tallest a blade grows.
fn tallest() -> f32 {
  return grow.shape.x * max(1.0 + grow.shape.y * 0.5, 0.05);
}

// The blades being kept by a workgroup's tile.
var<workgroup> keptCount: atomic<u32>;

// Builds a tile (one a workgroup, tiles[0..tiles.x)): each of its cells'
// blades as cs_main has it but for what depends on the camera, kept if it
// grows there at all and ranks below the tile's threshold.
@compute @workgroup_size(256, 1, 1)
fn cs_build(@builtin(workgroup_id) group: vec3u, @builtin(local_invocation_index) lane: u32) {
  let tile = tiles[group.x];
  if (lane == 0u) {
    atomicStore(&keptCount, 0u);
  }
  workgroupBarrier();
  for (var i = lane; i < kTileCells * kTileCells; i += 256u) {
    buildCell(tile, i);
  }
  workgroupBarrier();
  if (lane == 0u) {
    tileState[tile.slot] = vec4u(min(atomicLoad(&keptCount), tile.room), 0u, 0u, 0u);
  }
}

fn buildCell(tile: TileEntry, i: u32) {
  let lattice = tile.lattice & 3u;
  let cell = tile.origin + vec2i(i32(i % kTileCells), i32(i / kTileCells));
  if (lattice < 3u && (cell.x & 1) == 0 && (cell.y & 1) == 0) {
    return; // the next lattice's
  }
  count(14u);
  var seed = pcg(bitcast<u32>(cell.x) ^ pcg(bitcast<u32>(cell.y) ^ pcg(lattice + 40503u)));
  let size = grow.shape.w * f32(1u << lattice);
  let local = (vec2f(cell) + vec2f(random(&seed), random(&seed))) * size;
  let at = (local - grow.ground.xy) / grow.ground.zw;
  let texel = vec2i(floor(at));
  if (any(texel < vec2i(0)) || any(texel >= grow.groundSize.xy)) {
    return;
  }
  let ground = textureLoad(groundMap, texel, 0);
  if (!hasGround(ground)) {
    return;
  }
  if (rankOf(lattice, seed) >= tile.threshold) {
    return;
  }
  let height = heightAt(at, ground.w);
  let uv = (local - grow.maps.xy) * grow.maps.zw;
  let maps = grassShape(uv);
  let keep = random(&seed);
  let tall = random(&seed);
  if (keep >= maps.r * step(0.5, maps.b)) {
    return;
  }
  let full = grow.shape.x * max(1.0 + grow.shape.y * (tall * 2.0 - 1.0) * 0.5, 0.05) * maps.g;
  if (full < 0.004) {
    return; // never tall enough to draw
  }
  let index = atomicAdd(&keptCount, 1u);
  if (index >= tile.room) {
    count(16u);
    return;
  }
  count(15u);
  let normal = normalize((grow.normalMatrix * vec4f(ground.xyz, 0.0)).xyz);
  let tall12 = u32(round(clamp(full / tallest(), 0.0, 1.0) * 4095.0));
  let world = (grow.model * vec4f(local, height, 1.0)).xy;
  let detail = u32(round(clamp(vigourDetailAt(world), 0.0, 1.0) * 255.0));
  cache[tile.first + index] = vec4u(i | (tall12 << 12u) | (detail << 24u), bitcast<u32>(height),
                                    bitcast<u32>(ground.w), pack2x16snorm(normal.xy));
}

// A blade's rank (see cs_main), from its cell's seed past its place.
fn rankOf(lattice: u32, seed: u32) -> f32 {
  let top = select(1.0 / 64.0, 1.0 / f32(1u << (2u * lattice)), lattice < 3u);
  let bottom = select(0.0, top * 0.25, lattice < 3u);
  return mix(bottom, top, f32(pcg(seed ^ 0x68bc21ebu) >> 8u) / 16777216.0);
}

// --- A frame's blades, from the tiles in view ---------------------------------
// (tiles[tiles.y..][..tiles.z], a tile a workgroup, nearest first.) They're
// listed tile by tile, in the tiles' order: cs_count finds which of each
// tile's blades are drawn, and in which list (listedIn), and counts them;
// cs_order adds up those counts in the tiles' order, for where each tile's
// blades go in each list (the lists one after the other); cs_grow then grows
// those blades into their places. A tile's blades are kept in rows, from its
// low Y: they're gone through a row or so at a time from the end nearer the
// camera, so they're listed about front to back within it too.

// A tile's blades drawn so far, in each list; whether it's hidden.
var<workgroup> listed: array<atomic<u32>, 3>;
var<workgroup> tileHidden: u32;

@compute @workgroup_size(64, 1, 1)
fn cs_count(@builtin(workgroup_id) group: vec3u, @builtin(local_invocation_index) lane: u32) {
  let nth = group.x + group.y * 65535u;
  if (nth >= grow.tiles.z) {
    return;
  }
  let tile = tiles[grow.tiles.y + nth];
  if (lane < 3u) {
    atomicStore(&listed[lane], 0u);
  }
  if (lane == 0u) {
    tileHidden = select(0u, 1u, hiddenTile(tile));
  }
  let hidden = workgroupUniformLoad(&tileHidden) != 0u;
  let held = tileState[tile.slot].x;
  let first = tile.firstChunk * kChunk;
  for (var i = lane; i < held; i += kChunk) {
    var b: Grown;
    var list = 0u;
    if (!hidden) {
      if (grownFrom(tile, i, &b, false)) {
        if (shown(b.root, b.height)) {
          list = min(u32(detailOf(b.away, b.height).y), 2u) + 1u;
          atomicAdd(&listed[list - 1u], 1u);
        }
      }
    }
    listedIn[first + i] = list;
  }
  workgroupBarrier();
  if (lane == 0u) {
    tileState[tile.slot] = vec4u(held, atomicLoad(&listed[0]), atomicLoad(&listed[1]),
                                 atomicLoad(&listed[2]));
  }
}

// Whether all of a tile is hidden behind what's drawn opaque: its blades'
// box (its ground's, with room for them to stand and lean, and for their
// shadows) nearer nowhere than the farthest of what's over it on the
// screen. Not if it reaches behind the camera, nor if it's near enough that
// its blades cast shadows when they'd fall too far to tell.
fn hiddenTile(tile: TileEntry) -> bool {
  let levels = i32(grow.hidden.z);
  if (levels <= 0) {
    return false;
  }
  let lattice = tile.lattice & 3u;
  let size = grow.shape.w * f32(1u << lattice);
  let lean = tallest() * 1.2;
  var spread = lean;
  if (grow.detail.z > 0.0 && grow.forward.w >= 0.0) {
    spread += lean * grow.forward.w;
  }
  let across = min(length(grow.model[0].xyz), length(grow.model[1].xyz));
  let up = length(grow.model[2].xyz);
  let low = vec3f(vec2f(tile.origin) * size - spread / across, tile.heights.x);
  let high = vec3f(vec2f(tile.origin + vec2i(i32(kTileCells))) * size + spread / across,
                   tile.heights.y + lean / up);
  var nearest = 3.4e38;
  var lowest = vec2f(3.4e38);
  var highest = vec2f(-3.4e38);
  for (var corner = 0u; corner < 8u; corner++) {
    let own = select(low, high, vec3<bool>((corner & 1u) != 0u, (corner & 2u) != 0u,
                                           (corner & 4u) != 0u));
    let world = (grow.model * vec4f(own, 1.0)).xyz;
    nearest = min(nearest, dot(world - grow.eye.xyz, grow.forward.xyz));
    let clip = grow.viewProjection * vec4f(world, 1.0);
    if (clip.w < 1e-3) {
      return false;
    }
    let at = vec2f(clip.x, -clip.y) / clip.w * 0.5 + 0.5;
    lowest = min(lowest, at);
    highest = max(highest, at);
  }
  if (nearest < grow.detail.z && grow.forward.w < 0.0) {
    return false;
  }
  // The blocks it's over (and one more round them, for the depths being
  // found at points, not all over each block), at the finest level where
  // they're few.
  let finest = vec2i(grow.hidden.xy);
  var lo = clamp(vec2i(floor(lowest * grow.hidden.xy)) - 1, vec2i(0), finest - 1);
  var hi = clamp(vec2i(floor(highest * grow.hidden.xy)) + 1, vec2i(0), finest - 1);
  var level = 0;
  while (level < levels - 1 && any(hi - lo > vec2i(3))) {
    level++;
    lo = lo >> vec2u(1u);
    hi = hi >> vec2u(1u);
  }
  if (any(hi - lo > vec2i(3))) {
    return false;
  }
  let most = vec2i(textureDimensions(farthest, level)) - 1;
  var deepest = 0.0;
  for (var y = lo.y; y <= hi.y; y++) {
    for (var x = lo.x; x <= hi.x; x++) {
      deepest = max(deepest,
                    bitcast<f32>(textureLoad(farthest, min(vec2i(x, y), most), level).r));
    }
  }
  if (nearest > deepest) {
    count(17u);
    return true;
  }
  return false;
}

// The tiles' blades drawn in each list added up, in the tiles' order (one
// workgroup): where each tile's go in each list (tileState's yzw), and the
// lists one after the other, as far as there's room (draws).
var<workgroup> sums: array<vec3u, 256>;

@compute @workgroup_size(256, 1, 1)
fn cs_order(@builtin(local_invocation_index) lane: u32) {
  let tilesDrawn = grow.tiles.z;
  let each = (tilesDrawn + 255u) / 256u;
  let start = min(lane * each, tilesDrawn);
  let end = min(start + each, tilesDrawn);
  var own = vec3u(0u);
  for (var t = start; t < end; t++) {
    own += tileState[tiles[grow.tiles.y + t].slot].yzw;
  }
  sums[lane] = own;
  workgroupBarrier();
  for (var stride = 1u; stride < 256u; stride <<= 1u) {
    var before = vec3u(0u);
    if (lane >= stride) {
      before = sums[lane - stride];
    }
    workgroupBarrier();
    sums[lane] += before;
    workgroupBarrier();
  }
  let totals = sums[255];
  let starts = vec3u(0u, totals.x, totals.x + totals.y);
  var at = starts + sums[lane] - own;
  for (var t = start; t < end; t++) {
    let slot = tiles[grow.tiles.y + t].slot;
    let state = tileState[slot];
    tileState[slot] = vec4u(state.x, at);
    at += state.yzw;
  }
  if (lane == 0u) {
    let room = grow.capacity.x + grow.capacity.y + grow.capacity.z;
    for (var l = 0u; l < 3u; l++) {
      let first = min(starts[l], room);
      atomicStore(&draws[l * 4u + 1u], min(totals[l], room - first));
      atomicStore(&draws[64u * (l + 1u)], first);
    }
  }
}

// Grows the blades cs_count found are drawn into their places.
@compute @workgroup_size(64, 1, 1)
fn cs_grow(@builtin(workgroup_id) group: vec3u, @builtin(local_invocation_index) lane: u32) {
  let nth = group.x + group.y * 65535u;
  if (nth >= grow.tiles.z) {
    return;
  }
  if (lane < 3u) {
    atomicStore(&listed[lane], 0u);
  }
  workgroupBarrier();
  let tile = tiles[grow.tiles.y + nth];
  let state = tileState[tile.slot];
  let held = state.x;
  let chunks = (held + kChunk - 1u) / kChunk;
  let reversed = (tile.lattice & 16u) != 0u;
  let first = tile.firstChunk * kChunk;
  let room = grow.capacity.x + grow.capacity.y + grow.capacity.z;
  for (var k = 0u; k < chunks; k++) {
    let i = select(k, chunks - 1u - k, reversed) * kChunk + lane;
    if (i >= held) {
      continue;
    }
    let list = listedIn[first + i];
    if (list == 0u) {
      continue;
    }
    let at = state[list] + atomicAdd(&listed[list - 1u], 1u);
    if (at >= room) {
      count(9u); // out of room
      continue;
    }
    var b: Grown;
    _ = grownFrom(tile, i, &b, true);
    put(at, list - 1u, detailOf(b.away, b.height).y, b.root, b.height, b.width, b.away,
        b.normal, b.uv, tile.lattice & 3u, b.seed, tile.lattice & 12u, b.vigour, b.lean);
  }
}

// A tile's kept blade as it grows this frame.
struct Grown {
  root: vec3f,
  height: f32,
  width: f32,
  away: f32,
  uv: vec2f,
  normal: u32, // its ground's (KeptBlade::w)
  seed: u32,   // its cell's, past its place, rank and maps' randoms
  vigour: f32,
  lean: vec2f, // how it's laid (laidAt(): mown grass rolled)
}

// Grows a tile's kept blade `i` as cs_main would have it: whether it's kept
// this far away (and in its lattice's cells this frame), and if so (or, if
// `found`, as it was found it is) how it is. With `found`, its steps aren't
// counted again.
fn grownFrom(tile: TileEntry, i: u32, b: ptr<function, Grown>, found: bool) -> bool {
  if (!found) {
    count(0u);
  }
  let kept = cache[tile.first + i];
  let lattice = tile.lattice & 3u;
  let at = kept.x & 4095u;
  let cell = tile.origin + vec2i(i32(at % kTileCells), i32(at / kTileCells));
  let span = grow.lattices[lattice];
  if (!found && (any(cell < span.xy) || any(cell >= span.xy + span.zw))) {
    return false;
  }
  var seed = pcg(bitcast<u32>(cell.x) ^ pcg(bitcast<u32>(cell.y) ^ pcg(lattice + 40503u)));
  let size = grow.shape.w * f32(1u << lattice);
  let local = (vec2f(cell) + vec2f(random(&seed), random(&seed))) * size;
  let away = distance((grow.model * vec4f(local, bitcast<f32>(kept.z), 1.0)).xyz, grow.camera.xyz);
  let rank = rankOf(lattice, seed);
  let share = grassKept(away) * (1.0 + kGrowIn);
  if (!found && (away > grow.reach[lattice] || rank >= share)) {
    return false;
  }
  let grown = smoothstep(0.0, 1.0, log2(share / max(rank, 1e-9)) / log2(1.0 + kGrowIn));
  if (!found) {
    count(1u);
    count(2u);
  }
  let keep = random(&seed); // whether it grows here, and how tall
  let tall = random(&seed);
  (*b).root = (grow.model * vec4f(local, bitcast<f32>(kept.y), 1.0)).xyz;
  (*b).uv = (local - grow.maps.xy) * grow.maps.zw;
  // Its vigour: fewer and shorter where it's low.
  let vigour = vigourOf((*b).root.xy, f32(kept.x >> 24u) / 255.0);
  if (!found && !vigourKeeps(seed, vigour)) {
    return false;
  }
  (*b).vigour = vigour;
  let most = mix(grow.vigour.z, 1.0, vigour);
  if ((tile.lattice & 32u) != 0u) {
    // Over maps growing: as they are now (kept as they grow to).
    let maps = grassShapeNow((*b).uv, most);
    if (!found && keep >= maps.r * step(0.5, maps.b)) {
      return false;
    }
    let laid = laidAt((*b).uv);
    (*b).height = mownHeight(grow.shape.x * max(1.0 + grow.shape.y * (tall * 2.0 - 1.0) * 0.5, 0.05) * maps.g,
                             maps.g, tall, laid) * grown;
    (*b).lean = laid.xy;
  } else {
    (*b).lean = vec2f(0.0);
    (*b).height = f32((kept.x >> 12u) & 4095u) / 4095.0 * tallest() * most * grown;
  }
  (*b).width = grow.shape.z * min(pow(grassFull(away), -kWiden), kMostWider) * mix(0.4, 1.0, grown) *
               stubby((*b).height);
  (*b).away = away;
  (*b).normal = kept.w;
  (*b).seed = seed;
  return true;
}
