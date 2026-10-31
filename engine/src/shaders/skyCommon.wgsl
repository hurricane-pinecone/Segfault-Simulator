// Shared by the physical sky's passes (sky.wgsl), its background
// (skyBackground.wgsl) and the lit pass: directions <-> where they are in the
// sky's textures, which hold the whole sphere octahedrally (Z up: the upper
// half in the middle diamond, the lower folded into the corners).

// A unit direction's place in an octahedral texture (0..1).
fn octEncode(direction: vec3f) -> vec2f {
  let n = direction / (abs(direction.x) + abs(direction.y) + abs(direction.z));
  var p = n.xy;
  if (n.z < 0.0) {
    p = (vec2f(1.0) - abs(n.yx)) * select(vec2f(-1.0), vec2f(1.0), n.xy >= vec2f(0.0));
  }
  return p * 0.5 + vec2f(0.5);
}

// The direction a place in an octahedral texture holds (unit).
fn octDecode(uv: vec2f) -> vec3f {
  let f = uv * 2.0 - vec2f(1.0);
  var n = vec3f(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
  let t = max(-n.z, 0.0);
  n.x += select(t, -t, n.x >= 0.0);
  n.y += select(t, -t, n.y >= 0.0);
  return normalize(n);
}
