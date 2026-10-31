// Grass into the sun's shadow map: depth only, into the nearest cascade (or
// the one map), for the blades the grow pass listed as near enough to cast
// (meshGrassGrow.wgsl). Past them, a blade's shadow is less than a texel.
// (meshCommon and grassBlade.wgsl are prefixed.)
//
// Each is a strip of two segments along the blade's curve as the lit pass
// bends it (grassBlade.wgsl), so its shadow sways and flattens with it. It's
// turned square to the sun and drawn wider than the blade (grass.shadow.z),
// so it doesn't vanish as the blade turns edge-on, and a field's shadows hold
// together at the map's resolution rather than flicker. It's pushed back from
// the sun by about its own width, so a blade isn't shadowed by itself, and
// thins away toward the end of its reach.

struct SunCascade {
  viewProjection: mat4x4f,
}

@group(0) @binding(1) var<uniform> sunCascade: SunCascade;

@group(1) @binding(0) var<storage, read> blades: array<GrownBlade>;
@group(1) @binding(1) var<storage, read> casters: array<u32>; // their indices
@group(1) @binding(2) var<uniform> grass: Grass;
@group(1) @binding(3) var trampleMap: texture_2d<f32>;
@group(1) @binding(4) var trampleSampler: sampler;

@vertex
fn vs_main(@builtin(vertex_index) index: u32, @builtin(instance_index) instance: u32) -> @builtin(position) vec4f {
  let grown = blades[casters[instance]];
  let pressed = textureSampleLevel(trampleMap, trampleSampler,
                                   (grown.root.xy - grass.trample.xy) * grass.trample.zw, 0.0);
  let b = bladeAt(grown, grass.blade.x, pressed);

  // Two vertices a row (the root, half way), then the tip.
  let row = min(index / 2u, 2u);
  let t = f32(row) * 0.5;
  let side = select(f32(index % 2u) * 2.0 - 1.0, 0.0, index >= 4u);
  let center = bladeCenter(b, t);
  let along = bladeAlong(b, t);

  // Across it, square to the sun (its own across, where it points at it).
  let toSun = frame.sunDirection.xyz;
  let square = cross(toSun, along);
  let across = select(b.across, normalize(square), dot(square, square) > 0.04);
  let away = distance(b.root, frame.cameraPosition.xyz);
  let width = b.width * grass.shadow.z * (1.0 - smoothstep(grass.shadow.y * 0.7, grass.shadow.y, away));
  let back = b.width * 0.6 + b.height * 0.05;
  let position = center + across * (side * width * 0.5 * (1.0 - t * t)) - toSun * back;
  return sunCascade.viewProjection * vec4f(position, 1.0);
}
