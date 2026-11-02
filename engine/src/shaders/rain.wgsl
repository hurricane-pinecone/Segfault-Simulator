// Rain (RainSettings, while the weather has it): streaks falling through the
// air around the camera and drops splashing where they land, drawn last in
// the lit pass, blended over what's there and behind what's in front (depth
// tested, not written). One draw: frame.rainDepth.z streaks, then .w
// splashes, then frame.rainDrips.x drips off edges, each a quad worked out
// here from its index (no buffers). Nothing
// falls or splashes where rain's overhead map has something above it (a
// roof, a tree). (meshCommon, skyCommon, meshSurface, the standard surface,
// meshLit and fog are prefixed: this lights them as meshLit lights a
// surface.)
//
// Each pixel's alpha takes how much rain covers it (1 - that), which
// temporal anti-aliasing reads: streaks move too fast for its history.

struct RainOut {
  @builtin(position) clip: vec4f,
  @location(0) worldPosition: vec3f,
  @location(1) local: vec2f, // across (-1..1), and along (0..1: tail to head; a splash, up)
  @location(2) @interpolate(flat) params: vec4f, // x opacity, y 1 if a splash, z its age (0..1)
}

// How fast drops fall (m/s): a few millimetres across, near their terminal
// speed.
const RAIN_FALL: f32 = 8.0;

// A drop's own random numbers (0..1), `salt` for each.
fn rainHash(index: u32, salt: u32) -> f32 {
  var h = index * 0x9e3779b9u + salt * 0x85ebca6bu;
  h = (h ^ (h >> 16u)) * 0x7feb352du;
  h = (h ^ (h >> 15u)) * 0x846ca68bu;
  h = h ^ (h >> 16u);
  return f32(h >> 8u) * (1.0 / 16777215.0);
}

// A quad's corners, two triangles: x across (-1..1), y along (0..1).
fn rainCorner(vertex: u32) -> vec2f {
  var corners = array<vec2f, 6>(vec2f(-1.0, 0.0), vec2f(1.0, 0.0), vec2f(-1.0, 1.0), vec2f(-1.0, 1.0),
                                vec2f(1.0, 0.0), vec2f(1.0, 1.0));
  return corners[vertex % 6u];
}

// Nothing (a quad drawn as a point, off the screen).
fn rainHidden() -> RainOut {
  var out: RainOut;
  out.clip = vec4f(2.0, 2.0, 2.0, 1.0);
  return out;
}

// Where the camera looks across the ground (unit XY).
fn rainAhead() -> vec2f {
  let ahead = frame.cameraForward.xy;
  let along = length(ahead);
  return select(vec2f(1.0, 0.0), ahead / along, along > 1e-3);
}

// A point in clip space, `pixels` across the screen from `clip` along
// `direction` (screen pixels, unit).
fn rainNudge(clip: vec4f, direction: vec2f, pixels: f32) -> vec4f {
  let size = max(frame.rainVeil.yz, vec2f(1.0));
  return clip + vec4f(direction * (2.0 * pixels / size) * clip.w, 0.0, 0.0);
}

// Streak `i`: a drop falling through one of two boxes that move with the
// camera (the world's drops wrap through them, so they don't move with it):
// half in a near one around it, the others in a far one reaching ahead. Its
// quad runs from where it was a streak's length ago (its blur as it falls)
// to where it is, across the screen at least a pixel wide (fainter if it's
// thinner, though less than its width alone would make it).
fn rainStreak(i: u32, vertex: u32) -> RainOut {
  let near = (i & 1u) == 0u;
  let size = select(vec3f(70.0, 70.0, 45.0), vec3f(24.0, 24.0, 22.0), near);
  let camera = frame.cameraPosition.xyz;
  let center = camera + vec3f(rainAhead() * select(28.0, 4.0, near), -0.3 * size.z);
  let low = center - 0.5 * size;
  let fall = RAIN_FALL * (0.8 + 0.4 * rainHash(i, 3u));
  // They drift on the wind as far as it has carried them (kept on the CPU,
  // RainDrift: a gust or a veer turns them, it doesn't jump the whole field
  // of drops), and slant in it as it is now (eased); they fall at their
  // own speed.
  let velocity = vec3f(frame.rainDrift.zw * 0.8, -fall);
  let seed = vec3f(rainHash(i, 0u), rainHash(i, 1u), rainHash(i, 2u));
  let travelled = vec3f(frame.rainDrift.xy, -fall * frame.time.x);
  let inBox = fract(seed + travelled / size - low / size);
  let head = low + inBox * size;

  // Under a roof (or below the ground), none.
  if (head.z < rainSurfaceHeight(head.xy)) {
    return rainHidden();
  }
  // Faded in at the box's sides (so drops don't pop in), near the camera and
  // far off.
  let toEdge = min(inBox, vec3f(1.0) - inBox) * size;
  let range = length(head - camera);
  var opacity = smoothstep(0.0, 2.0, min(min(toEdge.x, toEdge.y), toEdge.z)) * smoothstep(0.5, 1.5, range) *
                (1.0 - smoothstep(25.0, 45.0, range));

  let tail = head - normalize(velocity) * (frame.rainStreaks.x * fall / RAIN_FALL);
  // Plainer close by, where a drop's blur is long and bright.
  return rainLine(tail, head, frame.rainStreaks.y, opacity * mix(0.55, 0.35, smoothstep(3.0, 15.0, range)), vertex);
}

// A falling drop's quad, from `tail` to `head` (world), `width` metres
// across: on the screen at least a pixel wide (fainter if it's thinner,
// though less than its width alone would make it), and half a width past
// each end, so a short one is still round. `opacity` at its full width.
fn rainLine(tail: vec3f, head: vec3f, width: f32, opacity: f32, vertex: u32) -> RainOut {
  let camera = frame.cameraPosition.xyz;
  let headClip = frame.viewProjection * vec4f(head, 1.0);
  let tailClip = frame.viewProjection * vec4f(tail, 1.0);
  if (headClip.w < 0.1 || tailClip.w < 0.1 || opacity <= 0.0) {
    return rainHidden();
  }
  let screen = max(frame.rainVeil.yz, vec2f(1.0)) * 0.5;
  let headPixel = headClip.xy / headClip.w * screen;
  let tailPixel = tailClip.xy / tailClip.w * screen;
  let along = headPixel - tailPixel;
  let direction = select(vec2f(0.0, 1.0), normalize(along), length(along) > 1e-3);
  let across = vec2f(-direction.y, direction.x);
  // How wide it is on the screen: its width, a drop's distance away.
  let travel = head - tail;
  let heading = select(vec3f(0.0, 0.0, -1.0), normalize(travel), length(travel) > 1e-5);
  let crossing = cross(heading, normalize(camera - head));
  let side = select(vec3f(1.0, 0.0, 0.0), normalize(crossing), length(crossing) > 1e-4);
  let sideClip = frame.viewProjection * vec4f(head + side * width, 1.0);
  let wide = length(sideClip.xy / sideClip.w * screen - headPixel);
  let pixels = max(wide, 1.0);

  let corner = rainCorner(vertex);
  var out: RainOut;
  let clip = mix(tailClip, headClip, corner.y);
  let ends = (corner.y * 2.0 - 1.0) * pixels * 0.5;
  out.clip = rainNudge(rainNudge(clip, across, corner.x * pixels * 0.5), direction, ends);
  out.worldPosition = mix(tail, head, corner.y);
  out.local = corner;
  out.params = vec4f(opacity * pow(wide / pixels, 0.6), 0.0, 0.0, 0.0);
  return out;
}

// Splash `i`: a drop landing somewhere on the ground in a box ahead of the
// camera, every third of a second somewhere else: a crown of droplets
// thrown up and a ring spreading, facing the camera. The box wraps over the
// world (as streaks' and drips' do), so a splash stays where it landed as
// the camera moves. Only on what faces up (not a wall's top edge, a roof's
// ridge).
fn rainSplash(i: u32, vertex: u32) -> RainOut {
  let t = frame.time.x / 0.35 + rainHash(i, 10u);
  let landing = i * 7919u + u32(floor(t)) * 104729u;
  let age = fract(t);
  let camera = frame.cameraPosition.xyz;
  let size = 60.0;
  let low = camera.xy + rainAhead() * 12.0 - vec2f(0.5 * size);
  let seed = vec2f(rainHash(landing, 11u), rainHash(landing, 12u));
  let spot = low + fract(seed - low / size) * size;
  let height = rainSurfaceHeight(spot);
  if (height < -1e8) {
    return rainHidden();
  }
  let dx = rainSurfaceHeight(spot + vec2f(0.15, 0.0)) - rainSurfaceHeight(spot - vec2f(0.15, 0.0));
  let dy = rainSurfaceHeight(spot + vec2f(0.0, 0.15)) - rainSurfaceHeight(spot - vec2f(0.0, 0.15));
  if (abs(dx) > 0.15 || abs(dy) > 0.15) {
    return rainHidden();
  }
  let base = vec3f(spot, height);
  let range = length(base - camera);
  let opacity = 1.0 - smoothstep(22.0, 29.0, range);
  if (opacity <= 0.0) {
    return rainHidden();
  }
  return rainSplashAt(base, age, 0.05 + 0.05 * rainHash(landing, 13u), opacity, vertex);
}

// A splash's quad at `base` (world), `size` metres high and half as wide,
// facing the camera, `age` (0..1) through it.
fn rainSplashAt(base: vec3f, age: f32, size: f32, opacity: f32, vertex: u32) -> RainOut {
  let toCamera = frame.cameraPosition.xy - base.xy;
  let facing = select(vec2f(1.0, 0.0), normalize(toCamera), length(toCamera) > 1e-3);
  let across = vec3f(-facing.y, facing.x, 0.0);
  let corner = rainCorner(vertex);
  var out: RainOut;
  out.worldPosition = base + across * (corner.x * size) + vec3f(0.0, 0.0, corner.y * size * 1.2 + 0.01);
  out.clip = frame.viewProjection * vec4f(out.worldPosition, 1.0);
  out.local = corner;
  out.params = vec4f(opacity, 1.0, age, 0.0);
  return out;
}

// Drips (frame.rainDrips) are found on the overhead map: where what's drawn
// steps down more than 0.6 m to the open ground beside it (the edge of a
// roof, an eave, an awning, a canopy's rim) and not a slope, the rain
// running off it gathers into drops that fall to the ground and splash.
// Each of frame.rainDrips.x places around the camera looks along a line
// for such an edge (most find none, and draw nothing), and drips from it
// a few times before it looks somewhere else.

// How fast drips fall (m/s each second).
const DRIP_GRAVITY: f32 = 9.81;
// How long a drop gathers at the edge, the longest it falls (from about
// 7 m; higher ones aren't drawn) and its splash lasts (s): a drip's round.
const DRIP_GATHER: f32 = 0.35;
const DRIP_FALL: f32 = 1.2;
const DRIP_SPLASH: f32 = 0.3;

// An edge water drips off: just outside it (at), its height (top) and the
// ground's under it (ground).
struct DripEdge {
  found: bool,
  at: vec2f,
  top: f32,
  ground: f32,
}

// The first edge from `p` along `direction` (up to 4.8 m), going down or
// up, found to a centimetre; mostly where water runs off it (the roof falls
// toward it, is flat, or is thin), a fifth as often off a gable's sloping
// side, which water mostly runs along.
fn rainDripEdge(p: vec2f, direction: vec2f) -> DripEdge {
  var out = DripEdge(false, p, 0.0, 0.0);
  let stride = 0.15;
  var last = rainSurfaceHeight(p);
  if (last < -1e8) {
    return out;
  }
  var high = p;
  var low = p;
  var hit = false;
  for (var k = 1; k <= 32; k++) {
    let q = p + direction * (f32(k) * stride);
    let h = rainSurfaceHeight(q);
    if (h < -1e8) {
      return out;
    }
    if (last - h > 0.6) {
      high = q - direction * stride;
      low = q;
      hit = true;
      break;
    }
    if (h - last > 0.6) {
      high = q;
      low = q - direction * stride;
      hit = true;
      break;
    }
    last = h;
  }
  if (!hit) {
    return out;
  }
  let top = rainSurfaceHeight(high);
  let ground = rainSurfaceHeight(low);
  for (var b = 0; b < 4; b++) {
    let middle = (high + low) * 0.5;
    if (rainSurfaceHeight(middle) - ground > 0.5 * (top - ground)) {
      high = middle;
    } else {
      low = middle;
    }
  }
  // The edge's outward normal: down the step across it (the line looked
  // along may cross it at a slant).
  let e = 0.08;
  let across = vec2f(rainSurfaceHeight(high + vec2f(e, 0.0)) - rainSurfaceHeight(high - vec2f(e, 0.0)),
                     rainSurfaceHeight(high + vec2f(0.0, e)) - rainSurfaceHeight(high - vec2f(0.0, e)));
  let outward = select(normalize(low - high + direction * 1e-4), -normalize(across), length(across) > 0.3);
  // Which way the roof falls a little in from its edge (its sides off the
  // roof, at a corner, left out): water runs off where it falls toward the
  // edge, it's about flat, or it's thin.
  let base = high - outward * 0.2;
  let here = rainSurfaceHeight(base);
  var falls = vec2f(0.0);
  for (var k = 0; k < 4; k++) {
    let axis = select(vec2f(0.0, 1.0), vec2f(1.0, 0.0), k < 2);
    let sign = select(-1.0, 1.0, (k & 1) == 0);
    let h = rainSurfaceHeight(base + axis * (0.2 * sign));
    falls -= axis * (sign * select(h, here, here - h > 0.4) - sign * here) / 0.2;
  }
  let slope = length(falls);
  let thin = here - rainSurfaceHeight(base - outward * 0.3) > 0.6;
  if (!thin && slope > 0.08 && dot(falls, outward) < 0.5 * slope &&
      rainHash(u32(abs(high.x * 97.0 + high.y * 61.0) * 16.0), 28u) > 0.2) {
    return out;
  }
  out.found = true;
  out.at = low + outward * 0.02;
  out.top = rainSurfaceHeight(high) - 0.03;
  out.ground = rainSurfaceHeight(out.at + outward * 0.05);
  return out;
}

// Drip `i`: a place a few metres around the camera's view (the world's, as
// streaks' are: it stays put as the camera moves), looking for an edge in
// a direction of its own; then, each round, a drop gathering there, falling
// (strung out into a broken trickle in a downpour) and splashing below, if
// it drips this round (frame.rainDrips.y of them do).
fn rainDrip(i: u32, vertex: u32) -> RainOut {
  let round = DRIP_GATHER + DRIP_FALL + DRIP_SPLASH;
  let t = frame.time.x / round + rainHash(i, 20u);
  let count = u32(floor(t));
  let phase = fract(t) * round;
  if (rainHash(i * 7919u + count * 104729u, 24u) > frame.rainDrips.y) {
    return rainHidden();
  }
  // The same place for four rounds (a point dripping), each moving on in
  // its own round.
  let place = (count + i % 4u) / 4u;
  let key = i * 7919u + place * 104729u;
  let size = 32.0;
  let low = frame.cameraPosition.xy + rainAhead() * 9.0 - vec2f(0.5 * size);
  let seed = vec2f(rainHash(key, 21u), rainHash(key, 22u));
  let p = low + fract(seed - low / size) * size;
  let angle = rainHash(key, 23u) * 6.2831853;
  let edge = rainDripEdge(p, vec2f(cos(angle), sin(angle)));
  if (!edge.found) {
    return rainHidden();
  }
  let drop = edge.top - edge.ground;
  let fall = sqrt(2.0 * max(drop, 0.0) / DRIP_GRAVITY);
  if (fall > DRIP_FALL) {
    return rainHidden();
  }
  let camera = frame.cameraPosition.xyz;
  let range = length(vec3f(edge.at, edge.top) - camera);
  let opacity = smoothstep(0.3, 1.0, range) * (1.0 - smoothstep(18.0, 26.0, range));
  if (opacity <= 0.0) {
    return rainHidden();
  }
  let width = 0.005;
  if (phase < DRIP_GATHER) {
    // Gathering: a bead swelling under the edge.
    let grown = phase / DRIP_GATHER;
    let bead = width * (0.5 + 0.5 * grown);
    let at = vec3f(edge.at, edge.top - bead);
    var out = rainLine(at + vec3f(0.0, 0.0, bead), at - vec3f(0.0, 0.0, bead), bead, opacity * 0.8 * grown, vertex);
    out.params.y = 2.0;
    return out;
  }
  let falling = phase - DRIP_GATHER;
  if (falling < fall) {
    // Falling, blurred as long as it moves in a frame or so; in a downpour,
    // strung out behind into a trickle broken into drops.
    let speed = DRIP_GRAVITY * falling;
    let head = vec3f(edge.at, edge.top - 0.5 * DRIP_GRAVITY * falling * falling);
    let trickle = frame.rainDrips.z * min(0.9, edge.top - head.z) * (0.5 + 0.5 * rainHash(key, 25u));
    let blur = max(speed * 0.03, 2.0 * width);
    let tail = head + vec3f(0.0, 0.0, blur + trickle);
    // (A trickle's stream is thicker than a drop.)
    var out = rainLine(tail, head, width * (1.0 + 0.8 * frame.rainDrips.z), opacity, vertex);
    out.params.y = 2.0;
    out.params.z = trickle / 0.08; // drops along it
    out.params.w = rainHash(key + count, 26u);
    return out;
  }
  let age = (falling - fall) / DRIP_SPLASH;
  if (age >= 1.0) {
    return rainHidden();
  }
  // Bigger than a raindrop's: it gathered, and fell from higher.
  return rainSplashAt(vec3f(edge.at, edge.ground), age, 0.08 + 0.05 * rainHash(key, 27u), opacity, vertex);
}

@vertex
fn vs_rain(@builtin(vertex_index) vertex: u32, @builtin(instance_index) instance: u32) -> RainOut {
  let streaks = u32(frame.rainDepth.z);
  if (instance < streaks) {
    return rainStreak(instance, vertex);
  }
  let splashes = streaks + u32(frame.rainDepth.w);
  if (instance < splashes) {
    return rainSplash(instance - streaks, vertex);
  }
  return rainDrip(instance - splashes, vertex);
}

// The light a drop shows: the sky's (from above, and from behind it, which
// it bends toward the eye), the sun's (brightest looking toward it) and the
// lamps', as a surface is lit (dim at night: as dark as the night is).
fn rainLight(worldPosition: vec3f, pixel: vec2f) -> vec3f {
  let toView = normalize(frame.cameraPosition.xyz - worldPosition);
  let up = vec3f(0.0, 0.0, 1.0);
  if (frame.skyMode.x < 0.5) {
    let lamps = pointLighting(pixel, worldPosition, toView, up, Shine(toView, vec3f(0.02), 0.3, 1.0));
    return min(frame.sky.rgb * frame.sunColor.w + lamps.diffuse, vec3f(frame.ambientBalance.w));
  }
  let sky = (skyIrradianceAt(up) + skyIrradianceAt(-toView)) * 0.5 * frame.ambientBalance.rgb;
  let toSun = frame.sunDirection.xyz;
  let ahead = max(dot(-toView, toSun), 0.0);
  let sun = frame.sunLight.rgb * frame.sunBalance.rgb * (0.05 + 1.5 * pow(ahead, 12.0)) *
            sunVisibility(worldPosition, up) * cloudShadowAt(worldPosition);
  let lamp = frame.skyLight.z;
  let lamps = pointLighting(pixel, worldPosition, toView, up, Shine(toView, vec3f(0.02), 0.3, 1.0));
  // Lightning: drops shine toward the eye with a flash beyond them (they
  // bend its light forward), and catch the glowing clouds' light.
  let strike = lightningLighting(worldPosition, -toView, -toView, toView, vec3f(0.02), 0.3, 1.0);
  return min(sky + sun + lamps.diffuse * (2.0 * lamp) + strike.diffuse * 0.6, vec3f(frame.ambientBalance.w));
}

@fragment
fn fs_rain(in: RainOut) -> LitOut {
  var coverage = 0.0;
  if (in.params.y < 0.5) {
    // A streak: soft across, tapering at its ends.
    let x = in.local.x;
    coverage = (1.0 - x * x) * smoothstep(0.0, 0.25, in.local.y) * (1.0 - smoothstep(0.75, 1.0, in.local.y));
  } else if (in.params.y > 1.5) {
    // A drip: as a streak, its trickle (params.z drops along it, from its
    // head) broken into beads.
    let x = in.local.x;
    coverage = (1.0 - x * x) * smoothstep(0.0, 0.1, in.local.y) * (1.0 - smoothstep(0.8, 1.0, in.local.y));
    if (in.params.z > 1.0) {
      let along = (1.0 - in.local.y) * in.params.z + in.params.w;
      let bead = abs(fract(along) - 0.5) * 2.0;
      let first = 1.0 / in.params.z; // the head itself stays whole
      coverage *= max(1.0 - smoothstep(0.35, 0.75, bead), 1.0 - smoothstep(first, 2.0 * first, 1.0 - in.local.y)) *
                  mix(1.0, 0.5, in.local.y);
    }
  } else {
    // A splash: droplets thrown out and up in an arc, falling back, and a
    // ring spreading over the ground; both fading.
    let age = in.params.z;
    let p = vec2f(in.local.x, in.local.y);
    var drops = 0.0;
    for (var k = 0; k < 5; k++) {
      let a = (f32(k) + 0.5) / 5.0 * 3.14159265;
      let at = vec2f(cos(a), sin(a) * 0.8) * (0.25 + 0.7 * age) - vec2f(0.0, 0.6 * age * age);
      let r = 0.14 * (1.0 - 0.6 * age);
      drops = max(drops, 1.0 - smoothstep(r * 0.5, r, length(p - vec2f(0.0, 0.05) - at)));
    }
    let off = (abs(p.x) - (0.2 + 0.8 * age)) / 0.15;
    let ring = exp(-off * off) * (1.0 - smoothstep(0.0, 0.2, p.y));
    let fade = pow(1.0 - age, 1.5);
    coverage = max(drops, ring * 0.6) * fade * 0.6;
  }
  let alpha = clamp(coverage * in.params.x, 0.0, 1.0);
  if (alpha <= 0.002) {
    discard;
  }

  var color = rainLight(in.worldPosition, in.clip.xy) * frame.rainStreaks.z;
  // Into the haze, fog and veil, as surfaces.
  let away = in.worldPosition - frame.cameraPosition.xyz;
  if (frame.skyMode.x > 0.5) {
    color = mix(color, hazeSky(in.worldPosition), hazeAt(in.worldPosition));
  } else {
    color = mix(color, frame.sky.rgb, hazeAt(in.worldPosition));
  }
  if (frame.fogLight.z > 0.5) {
    color = mix(color, fogColor(away), fogAt(in.worldPosition));
  }
  color = mix(color, fogColor(away), rainVeilAt(in.worldPosition));
  // The scene's alpha keeps 1 - the rain over it (the pipeline blends it so).
  return LitOut(vec4f(color, alpha), vec4f(vec3f(0.0), alpha));
}
