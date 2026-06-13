// Renders ballistic debris as real voxel cubes, depth-tested against the
// raymarched world/bodies (shared planar view-depth). One instance per pool slot,
// 36 verts per cube; dead slots and behind-camera cubes collapse to a clipped
// vertex. Faces get the same directional shading as world voxels.
struct Camera {
  p0 : vec4<f32>, // camPos.xyz, worldSize
  p1 : vec4<f32>, // rayForward.xyz, width
  p2 : vec4<f32>, // rayRight.xyz, height
  p3 : vec4<f32>, // rayUp.xyz, time
};
@group(0) @binding(0) var<uniform> cam : Camera;
@group(0) @binding(1) var<storage, read> debris : array<Debris>;
@group(0) @binding(2) var<storage, read> materials : array<Material>;

struct VsOut {
  @builtin(position) pos : vec4<f32>,
  @location(0) col : vec3<f32>,
};

const HS : f32 = 0.55; // cube half-size (voxels)
const CLIPPED : vec4<f32> = vec4<f32>(2.0, 2.0, 2.0, 1.0);
const LIGHT : vec3<f32> = vec3<f32>(0.4, 0.9, 0.3);

@vertex
fn vs_main(@builtin(vertex_index) vid : u32,
           @builtin(instance_index) inst : u32) -> VsOut {
  var o : VsOut;
  o.col = vec3<f32>(0.0);
  let d = debris[inst];
  if (d.a.w <= 0.0) { o.pos = CLIPPED; return o; } // free slot
  let v = bitcast<u32>(d.b.w);
  if (matId(v) == 0u) { o.pos = CLIPPED; return o; } // air/unset material -> skip

  // Unit cube: 6 faces x 2 triangles, corner offsets in {-1,+1}, per-face normals.
  var corners = array<vec3<f32>, 36>(
      vec3<f32>(1.0, -1.0, -1.0), vec3<f32>(1.0, 1.0, -1.0), vec3<f32>(1.0, 1.0, 1.0),
      vec3<f32>(1.0, -1.0, -1.0), vec3<f32>(1.0, 1.0, 1.0), vec3<f32>(1.0, -1.0, 1.0),
      vec3<f32>(-1.0, -1.0, -1.0), vec3<f32>(-1.0, 1.0, 1.0), vec3<f32>(-1.0, 1.0, -1.0),
      vec3<f32>(-1.0, -1.0, -1.0), vec3<f32>(-1.0, -1.0, 1.0), vec3<f32>(-1.0, 1.0, 1.0),
      vec3<f32>(-1.0, 1.0, -1.0), vec3<f32>(1.0, 1.0, -1.0), vec3<f32>(1.0, 1.0, 1.0),
      vec3<f32>(-1.0, 1.0, -1.0), vec3<f32>(1.0, 1.0, 1.0), vec3<f32>(-1.0, 1.0, 1.0),
      vec3<f32>(-1.0, -1.0, -1.0), vec3<f32>(1.0, -1.0, 1.0), vec3<f32>(1.0, -1.0, -1.0),
      vec3<f32>(-1.0, -1.0, -1.0), vec3<f32>(-1.0, -1.0, 1.0), vec3<f32>(1.0, -1.0, 1.0),
      vec3<f32>(-1.0, -1.0, 1.0), vec3<f32>(1.0, -1.0, 1.0), vec3<f32>(1.0, 1.0, 1.0),
      vec3<f32>(-1.0, -1.0, 1.0), vec3<f32>(1.0, 1.0, 1.0), vec3<f32>(-1.0, 1.0, 1.0),
      vec3<f32>(-1.0, -1.0, -1.0), vec3<f32>(1.0, 1.0, -1.0), vec3<f32>(1.0, -1.0, -1.0),
      vec3<f32>(-1.0, -1.0, -1.0), vec3<f32>(-1.0, 1.0, -1.0), vec3<f32>(1.0, 1.0, -1.0));
  var normals = array<vec3<f32>, 6>(
      vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(-1.0, 0.0, 0.0),
      vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.0, -1.0, 0.0),
      vec3<f32>(0.0, 0.0, 1.0), vec3<f32>(0.0, 0.0, -1.0));
  let nrm = normals[vid / 6u];
  let wp = d.a.xyz + HS * corners[vid];

  // Project into the ray camera: decompose (wp - camPos) into the (forward, right,
  // up) basis; screen ndc = (right/forward, up/forward); depth = planar view-depth.
  let dir = wp - cam.p0.xyz;
  let a = dot(dir, cam.p1.xyz) / dot(cam.p1.xyz, cam.p1.xyz);
  // `!(a > eps)` also rejects NaN (a stuck garbage cube), which `a <= eps` would
  // let through.
  if (!(a > 0.0001)) { o.pos = CLIPPED; return o; }
  let nx = (dot(dir, cam.p2.xyz) / dot(cam.p2.xyz, cam.p2.xyz)) / a;
  let ny = (dot(dir, cam.p3.xyz) / dot(cam.p3.xyz, cam.p3.xyz)) / a;
  let depth = clamp(a / (2.0 * f32(WG)), 0.0, 1.0);
  o.pos = vec4<f32>(nx, ny, depth, 1.0);

  let base = materials[matId(v)].color.rgb;
  let shade = max(dot(nrm, normalize(LIGHT)), 0.0) * 0.6 + 0.4;
  o.col = base * shade;
  return o;
}

@fragment
fn fs_main(f : VsOut) -> @location(0) vec4<f32> {
  return vec4<f32>(f.col, 1.0);
}
