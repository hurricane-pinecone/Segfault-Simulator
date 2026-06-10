// sampleVoxel3D -- WebGPU bootstrap.
//
// Moving the tiny-voxel 3D game off the OpenGL engine onto a WebGPU compute
// stack (brickmap + raymarched rendering + GPU cellular-automata, modelled on
// JorisAR/GDVoxelPlayground). Native via wgpu-native; surface from SDL2.
//
// Milestone 4: GPU WATER. A discrete-voxel cellular-automaton compute pass --
// each water voxel falls, else spreads, MOVING into a cleared destination
// buffer via atomic compare-exchange (each target claimed once => conserved,
// race-free), ping-ponged between two voxel buffers each frame. Voxels now
// carry a type in the low byte (0 air / 1 solid / 2 water). Scene: a walled
// pool with a block of water dropped in. Next: click-to-carve/spawn edits +
// brick-skip rendering.

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <sdl2webgpu.h>
#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h> // wgpu-native extras (wgpuDevicePoll)

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace
{
constexpr int kWorld = 512;
constexpr int kBrick = 8;
constexpr int kBG = kWorld / kBrick; // 64

struct Brick
{
  uint32_t occupancy;
  uint32_t pointer;
};

struct Vec3
{
  float x, y, z;
};
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 cross(Vec3 a, Vec3 b)
{
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 normalize(Vec3 v)
{
  float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  return l > 0.0f ? Vec3{v.x / l, v.y / l, v.z / l} : v;
}
Vec3 scale(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }

WGPUStringView sv(const char* s) { return WGPUStringView{s, WGPU_STRLEN}; }

void logStr(const char* label, WGPUStringView s)
{
  std::fprintf(stderr,
               "%s: %.*s\n",
               label,
               static_cast<int>(s.length),
               s.data ? s.data : "");
}

WGPUAdapter requestAdapterSync(WGPUInstance instance, WGPUSurface surface)
{
  struct Out
  {
    WGPUAdapter adapter = nullptr;
  } out;
  WGPURequestAdapterOptions opts = {};
  opts.compatibleSurface = surface;
  opts.powerPreference = WGPUPowerPreference_HighPerformance;
  WGPURequestAdapterCallbackInfo cb = {};
  cb.mode = WGPUCallbackMode_AllowProcessEvents;
  cb.callback = [](WGPURequestAdapterStatus status,
                   WGPUAdapter adapter,
                   WGPUStringView message,
                   void* ud1,
                   void*)
  {
    if (status == WGPURequestAdapterStatus_Success)
      static_cast<Out*>(ud1)->adapter = adapter;
    else
      logStr("requestAdapter failed", message);
  };
  cb.userdata1 = &out;
  wgpuInstanceRequestAdapter(instance, &opts, cb);
  return out.adapter;
}

WGPUDevice requestDeviceSync(WGPUAdapter adapter)
{
  struct Out
  {
    WGPUDevice device = nullptr;
  } out;
  // Request the adapter's full limits so big storage buffers (a 512^3 voxel
  // buffer is 512MB, past the 128MB default) are allowed.
  WGPULimits limits = {};
  wgpuAdapterGetLimits(adapter, &limits);
  WGPUDeviceDescriptor desc = {};
  desc.requiredLimits = &limits;
  // Enable GPU timestamp queries when the adapter supports them (benchmark
  // HUD).
  WGPUFeatureName feats[1] = {WGPUFeatureName_TimestampQuery};
  if (wgpuAdapterHasFeature(adapter, WGPUFeatureName_TimestampQuery))
  {
    desc.requiredFeatureCount = 1;
    desc.requiredFeatures = feats;
  }
  desc.uncapturedErrorCallbackInfo.callback = [](WGPUDevice const*,
                                                 WGPUErrorType type,
                                                 WGPUStringView message,
                                                 void*,
                                                 void*)
  {
    std::fprintf(stderr,
                 "WebGPU error (%d): %.*s\n",
                 static_cast<int>(type),
                 static_cast<int>(message.length),
                 message.data ? message.data : "");
  };
  WGPURequestDeviceCallbackInfo cb = {};
  cb.mode = WGPUCallbackMode_AllowProcessEvents;
  cb.callback = [](WGPURequestDeviceStatus status,
                   WGPUDevice device,
                   WGPUStringView message,
                   void* ud1,
                   void*)
  {
    if (status == WGPURequestDeviceStatus_Success)
      static_cast<Out*>(ud1)->device = device;
    else
      logStr("requestDevice failed", message);
  };
  cb.userdata1 = &out;
  wgpuAdapterRequestDevice(adapter, &desc, cb);
  return out.device;
}

WGPUShaderModule makeShader(WGPUDevice device, const char* code)
{
  WGPUShaderSourceWGSL wgsl = {};
  wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
  wgsl.code = sv(code);
  WGPUShaderModuleDescriptor desc = {};
  desc.nextInChain = &wgsl.chain;
  return wgpuDeviceCreateShaderModule(device, &desc);
}

WGPUBuffer makeBuffer(WGPUDevice device, uint64_t size, WGPUBufferUsage usage)
{
  WGPUBufferDescriptor d = {};
  d.usage = usage;
  d.size = size;
  return wgpuDeviceCreateBuffer(device, &d);
}

// Reads back 4 GPU timestamps (water begin/end, render begin/end, in ns) and
// converts the deltas to milliseconds.
struct TsReadback
{
  WGPUBuffer buffer;
  double* simMs;
  double* renderMs;
  double* totalMs;
  bool* busy;
};
void onTsMapped(WGPUMapAsyncStatus status, WGPUStringView, void* ud1, void*)
{
  auto* r = static_cast<TsReadback*>(ud1);
  if (status == WGPUMapAsyncStatus_Success)
  {
    const auto* ts = static_cast<const uint64_t*>(
        wgpuBufferGetConstMappedRange(r->buffer, 0, 32));
    if (ts)
    {
      *r->simMs = static_cast<double>(ts[1] - ts[0]) / 1.0e6;
      *r->renderMs = static_cast<double>(ts[3] - ts[2]) / 1.0e6;
      *r->totalMs = static_cast<double>(ts[3] - ts[0]) / 1.0e6;
    }
    wgpuBufferUnmap(r->buffer);
  }
  *r->busy = false;
}

// Shared WGSL prelude: voxel = (r<<24)|(g<<16)|(b<<8)|type. type 0 air/1
// solid/2 water.
const char* kCommon = R"WGSL(
const WG : i32 = 512;
const BG : i32 = 64;
const SEA : i32 = 96;
struct Brick { occupancy : u32, pointer : u32 };
// Voxel = (r<<24)|(g<<16)|(b<<8)|low. low: bits0-1 type (0 air/1 solid/2 water),
// bits2-3 = water's remembered flow direction (0 +x, 1 -x, 2 +z, 3 -z).
fn hash3(x : u32, y : u32, z : u32, f : u32) -> u32 {
  var h = x * 374761393u + y * 668265263u + z * 2246822519u + f * 3266489917u;
  h = (h ^ (h >> 13u)) * 1274126177u;
  return h ^ (h >> 16u);
}
)WGSL";

const char* kGenWGSL = R"WGSL(
@group(0) @binding(0) var<storage, read_write> voxels0 : array<u32>;
@group(0) @binding(1) var<storage, read_write> voxels1 : array<u32>;
@group(0) @binding(2) var<storage, read_write> bricks : array<Brick>;

fn hash22(p : vec2<f32>) -> f32 {
  return fract(sin(dot(p, vec2<f32>(127.1, 311.7))) * 43758.5453);
}
fn vnoise(p : vec2<f32>) -> f32 {
  let i = floor(p);
  let f = fract(p);
  let u = f * f * (3.0 - 2.0 * f);
  let a = hash22(i);
  let b = hash22(i + vec2<f32>(1.0, 0.0));
  let c = hash22(i + vec2<f32>(0.0, 1.0));
  let d = hash22(i + vec2<f32>(1.0, 1.0));
  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
fn fbm(p : vec2<f32>) -> f32 {
  var v = 0.0; var amp = 0.5; var fr = 1.0;
  for (var i = 0; i < 4; i = i + 1) { v = v + amp * vnoise(p * fr); fr = fr * 2.0; amp = amp * 0.5; }
  return v;
}
fn terrainHeight(x : i32, z : i32) -> i32 {
  let p = vec2<f32>(f32(x), f32(z)) * 0.013;
  return i32(44.0 + fbm(p) * 130.0 + fbm(p * 4.0) * 8.0); // broad foothills + fine
}
fn shade(r : i32, g : i32, b : i32, x : i32, y : i32, z : i32, ty : u32) -> u32 {
  let k = 0.86 + 0.22 * (f32(hash3(u32(x), u32(y), u32(z), 5u) & 0xFFFFu) / 65535.0);
  return (u32(clamp(i32(f32(r) * k), 0, 255)) << 24u)
       | (u32(clamp(i32(f32(g) * k), 0, 255)) << 16u)
       | (u32(clamp(i32(f32(b) * k), 0, 255)) << 8u) | ty;
}
fn ihash2(x : i32, z : i32) -> u32 {
  var h = u32(x) * 73856093u ^ u32(z) * 19349663u;
  h = (h ^ (h >> 15u)) * 2246822519u;
  return h ^ (h >> 13u);
}
fn treeAt(x : i32, y : i32, z : i32) -> u32 {
  let cell = 40;
  let canopyR = 9;
  let cgx = x / cell;
  let cgz = z / cell;
  for (var dz = -1; dz <= 1; dz = dz + 1) {
    for (var dx = -1; dx <= 1; dx = dx + 1) {
      let gx = cgx + dx;
      let gz = cgz + dz;
      let hh = ihash2(gx, gz);
      if ((hh & 1u) != 0u) { continue; }                       // ~half of cells
      let tx = gx * cell + 10 + i32(hh % 20u);
      let tz = gz * cell + 10 + i32((hh >> 10u) % 20u);
      let surf = terrainHeight(tx, tz);
      if (surf <= SEA + 4) { continue; }                       // grass only
      let topY = surf + 30 + i32((hh >> 20u) % 24u);
      if (abs(x - tx) <= 1 && abs(z - tz) <= 1 && y >= surf && y < topY) {
        return shade(101, 67, 33, x, y, z, 1u);                // trunk
      }
      let ddx = x - tx; let ddy = y - topY; let ddz = z - tz;
      if (ddx * ddx + ddy * ddy + ddz * ddz <= canopyR * canopyR) {
        return shade(54, 110, 48, x, y, z, 1u);                // canopy
      }
    }
  }
  return 0u;
}
fn sampleVoxel(x : i32, y : i32, z : i32) -> u32 {
  let h = terrainHeight(x, z);
  if (y < h) {
    let d = h - y;
    if (d <= 4) {
      if (h <= SEA + 4) { return shade(206, 192, 142, x, y, z, 1u); } // sand
      return shade(86, 168, 80, x, y, z, 1u);                        // grass
    }
    if (d <= 20) { return shade(122, 92, 60, x, y, z, 1u); }          // dirt
    return shade(108, 110, 124, x, y, z, 1u);                        // stone
  }
  if (y < SEA) {                                                     // lake water
    let dir = hash3(u32(x), u32(y), u32(z), 9u) & 3u;
    return shade(50, 110, 210, x, y, z, 2u | (dir << 2u));
  }
  if (y < h + 72) {                  // trees only live just above the surface
    let tree = treeAt(x, y, z);
    if (tree != 0u) { return tree; }
  }
  return 0u;
}

var<workgroup> occ : atomic<u32>;

@compute @workgroup_size(8, 8, 1)
fn generate(@builtin(local_invocation_id) lloc : vec3<u32>,
            @builtin(local_invocation_index) lidx : u32,
            @builtin(workgroup_id) wid : vec3<u32>) {
  let bi = u32(i32(wid.x) + i32(wid.y) * BG + i32(wid.z) * BG * BG);
  let lx = i32(lloc.x);
  let ly = i32(lloc.y);
  let bx = i32(wid.x) * 8 + lx;
  let by = i32(wid.y) * 8 + ly;
  let bz = i32(wid.z) * 8;
  if (lidx == 0u) { atomicStore(&occ, 0u); }
  workgroupBarrier();
  for (var lz = 0; lz < 8; lz = lz + 1) {
    let v = sampleVoxel(bx, by, bz + lz);
    let i = bi * 512u + u32(lx + ly * 8 + lz * 64);
    voxels0[i] = v;
    voxels1[i] = select(v, 0u, (v & 3u) == 2u); // static in both buffers; water only in buffer 0
    if (v != 0u) { atomicAdd(&occ, 1u); }
  }
  workgroupBarrier();
  if (lidx == 0u) {
    bricks[bi].occupancy = atomicLoad(&occ);
    bricks[bi].pointer = bi;
  }
}
)WGSL";

const char* kWaterWGSL = R"WGSL(
@group(0) @binding(0) var<storage, read> src : array<u32>;
@group(0) @binding(1) var<storage, read_write> dst : array<atomic<u32>>;
@group(0) @binding(2) var<storage, read> bricks : array<Brick>;
@group(0) @binding(3) var<uniform> frame : vec4<u32>;

fn vIndex(x : i32, y : i32, z : i32) -> u32 {
  let bi = u32((x / 8) + (y / 8) * BG + (z / 8) * BG * BG);
  return bricks[bi].pointer * 512u + u32((x % 8) + (y % 8) * 8 + (z % 8) * 64);
}
fn srcType(x : i32, y : i32, z : i32) -> u32 {
  if (x < 0 || y < 0 || z < 0 || x >= WG || y >= WG || z >= WG) { return 1u; } // wall
  return src[vIndex(x, y, z)] & 3u;
}
fn nbx(x : i32, d : u32) -> i32 { if (d == 0u) { return x + 1; } if (d == 1u) { return x - 1; } return x; }
fn nbz(z : i32, d : u32) -> i32 { if (d == 2u) { return z + 1; } if (d == 3u) { return z - 1; } return z; }

@compute @workgroup_size(8, 8, 1)
fn water(@builtin(local_invocation_id) lloc : vec3<u32>,
         @builtin(workgroup_id) wid : vec3<u32>) {
  let x = i32(wid.x) * 8 + i32(lloc.x);
  let y = i32(wid.y) * 8 + i32(lloc.y);
  for (var lz = 0; lz < 8; lz = lz + 1) {
    let z = i32(wid.z) * 8 + lz;
    let v = src[vIndex(x, y, z)];
    let ty = v & 3u;
    // Only water is simulated. Static terrain + air persist in dst already (it
    // is not cleared or copied), so non-water cells do nothing here.
    if (ty != 2u) { continue; }

    // Water (ty == 2). Fall if it can; each move claims a cleared destination
    // cell via compare-exchange (conserved, race-free).
    if (srcType(x, y - 1, z) == 0u) {
      let r = atomicCompareExchangeWeak(&dst[vIndex(x, y - 1, z)], 0u, v);
      if (r.exchanged) { continue; }
    }
    // Else keep flowing in the remembered direction until it's blocked...
    let dir = (v >> 2u) & 3u;
    let sx = nbx(x, dir);
    let sz = nbz(z, dir);
    if (srcType(sx, y, sz) == 0u) {
      let rs = atomicCompareExchangeWeak(&dst[vIndex(sx, y, sz)], 0u, v);
      if (rs.exchanged) { continue; }
    }
    // ...blocked: pick a new random direction and remember it.
    var moved = false;
    let pick = hash3(u32(x), u32(y), u32(z), frame.x) % 4u;
    for (var k = 0u; k < 4u; k = k + 1u) {
      let d = (pick + k) % 4u;
      let nx = nbx(x, d);
      let nz = nbz(z, d);
      if (srcType(nx, y, nz) == 0u) {
        let nv = (v & 0xFFFFFFF3u) | (d << 2u);
        let r2 = atomicCompareExchangeWeak(&dst[vIndex(nx, y, nz)], 0u, nv);
        if (r2.exchanged) { moved = true; break; }
      }
    }
    if (moved) { continue; }
    atomicStore(&dst[vIndex(x, y, z)], v); // couldn't move -> stay
  }
}
)WGSL";

const char* kRenderWGSL = R"WGSL(
struct Camera {
  p0 : vec4<f32>,  // camPos.xyz, worldSize
  p1 : vec4<f32>,  // rayForward.xyz, width
  p2 : vec4<f32>,  // rayRight.xyz, height
  p3 : vec4<f32>,  // rayUp.xyz, time
};
@group(0) @binding(0) var<uniform> cam : Camera;
@group(0) @binding(1) var<storage, read> voxels : array<u32>;
@group(0) @binding(2) var<storage, read> bricks : array<Brick>;

@vertex
fn vs_main(@builtin(vertex_index) vid : u32) -> @builtin(position) vec4<f32> {
  var p = array<vec2<f32>, 3>(vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(p[vid], 0.0, 1.0);
}

@fragment
fn fs_main(@builtin(position) fragPos : vec4<f32>) -> @location(0) vec4<f32> {
  let res = vec2<f32>(cam.p1.w, cam.p2.w);
  let ndc = vec2<f32>(fragPos.x / res.x * 2.0 - 1.0, 1.0 - fragPos.y / res.y * 2.0);
  let bg = mix(vec3<f32>(0.10, 0.12, 0.18), vec3<f32>(0.02, 0.02, 0.04), ndc.y * 0.5 + 0.5);

  let ro = cam.p0.xyz;
  let rd = normalize(cam.p1.xyz + ndc.x * cam.p2.xyz + ndc.y * cam.p3.xyz);
  let inv = vec3<f32>(1.0) / rd;

  let t0 = (vec3<f32>(0.0) - ro) * inv;
  let t1 = (vec3<f32>(f32(WG)) - ro) * inv;
  let tenter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z));
  let texit = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
  if (texit < max(tenter, 0.0)) { return vec4<f32>(bg, 1.0); }

  let tstart = max(tenter, 0.0) + 0.0001;
  let lightDir = normalize(vec3<f32>(0.4, 0.9, 0.3));
  let stepi = vec3<i32>(sign(rd));
  let stepf = vec3<f32>(sign(rd));
  let pos0 = max(stepf, vec3<f32>(0.0)); // 1 where ray goes +, else 0 (next face offset)

  // --- Outer DDA over bricks; empty bricks are skipped in one step. ---
  var brick = vec3<i32>(floor((ro + rd * tstart) / 8.0));
  var tMaxB = ((vec3<f32>(brick) + pos0) * 8.0 - ro) * inv;
  let tDeltaB = abs(inv) * 8.0;
  var tEnter = tstart;
  var bnorm = vec3<f32>(0.0, 1.0, 0.0);

  for (var ob = 0; ob < 210; ob = ob + 1) {
    if (brick.x < 0 || brick.y < 0 || brick.z < 0 || brick.x >= BG || brick.y >= BG || brick.z >= BG) { break; }
    let bidx = u32(brick.x + brick.y * BG + brick.z * BG * BG);
    if (bricks[bidx].occupancy != 0u) {
      let slot = bricks[bidx].pointer;
      let bmin = brick * 8;

      // --- Inner DDA over the brick's 8^3 voxels; ends on an integer bounds
      //     check (the voxel left the brick), not a fragile t-comparison. ---
      var voxel = clamp(vec3<i32>(floor(ro + rd * (tEnter + 0.0001))), bmin, bmin + vec3<i32>(7));
      var tMaxV = ((vec3<f32>(voxel) + pos0) - ro) * inv;
      var vnorm = bnorm;
      for (var iv = 0; iv < 26; iv = iv + 1) {
        let l = voxel - bmin;
        let v = voxels[slot * 512u + u32(l.x + l.y * 8 + l.z * 64)];
        if ((v & 3u) != 0u) {
          let col = vec3<f32>(f32((v >> 24u) & 255u), f32((v >> 16u) & 255u), f32((v >> 8u) & 255u)) / 255.0;
          let diff = max(dot(vnorm, lightDir), 0.0) * 0.7 + 0.3;
          return vec4<f32>(col * diff, 1.0);
        }
        if (tMaxV.x < tMaxV.y && tMaxV.x < tMaxV.z) {
          voxel.x += stepi.x; tMaxV.x += abs(inv.x); vnorm = vec3<f32>(-stepf.x, 0.0, 0.0);
          if (voxel.x < bmin.x || voxel.x > bmin.x + 7) { break; }
        } else if (tMaxV.y < tMaxV.z) {
          voxel.y += stepi.y; tMaxV.y += abs(inv.y); vnorm = vec3<f32>(0.0, -stepf.y, 0.0);
          if (voxel.y < bmin.y || voxel.y > bmin.y + 7) { break; }
        } else {
          voxel.z += stepi.z; tMaxV.z += abs(inv.z); vnorm = vec3<f32>(0.0, 0.0, -stepf.z);
          if (voxel.z < bmin.z || voxel.z > bmin.z + 7) { break; }
        }
      }
    }

    // Advance to the next brick.
    if (tMaxB.x < tMaxB.y && tMaxB.x < tMaxB.z) {
      brick.x += stepi.x; tEnter = tMaxB.x; tMaxB.x += tDeltaB.x; bnorm = vec3<f32>(-stepf.x, 0.0, 0.0);
    } else if (tMaxB.y < tMaxB.z) {
      brick.y += stepi.y; tEnter = tMaxB.y; tMaxB.y += tDeltaB.y; bnorm = vec3<f32>(0.0, -stepf.y, 0.0);
    } else {
      brick.z += stepi.z; tEnter = tMaxB.z; tMaxB.z += tDeltaB.z; bnorm = vec3<f32>(0.0, 0.0, -stepf.z);
    }
  }
  return vec4<f32>(bg, 1.0);
}
)WGSL";

// Cleanup (every frame, after the water tick): recompute each brick's occupancy
// from the NEW buffer (water moved between bricks, so brick-skip needs it
// fresh) AND wipe just the dynamic (water) voxels from the OLD buffer, so it is
// ready to be written next frame. Static terrain is never cleared or copied --
// it lives permanently in both buffers.
const char* kRecountWGSL = R"WGSL(
@group(0) @binding(0) var<storage, read> countBuf : array<u32>;       // new state
@group(0) @binding(1) var<storage, read_write> clearBuf : array<u32>; // old buffer
@group(0) @binding(2) var<storage, read_write> bricks : array<Brick>;
var<workgroup> occ : atomic<u32>;

@compute @workgroup_size(8, 8, 1)
fn recount(@builtin(local_invocation_id) lloc : vec3<u32>,
           @builtin(local_invocation_index) lidx : u32,
           @builtin(workgroup_id) wid : vec3<u32>) {
  let bi = u32(i32(wid.x) + i32(wid.y) * BG + i32(wid.z) * BG * BG);
  let slot = bricks[bi].pointer;
  let lx = i32(lloc.x);
  let ly = i32(lloc.y);
  if (lidx == 0u) { atomicStore(&occ, 0u); }
  workgroupBarrier();
  for (var lz = 0; lz < 8; lz = lz + 1) {
    let i = slot * 512u + u32(lx + ly * 8 + lz * 64);
    if ((clearBuf[i] & 3u) == 2u) { clearBuf[i] = 0u; }     // wipe water from the old buffer
    if ((countBuf[i] & 3u) != 0u) { atomicAdd(&occ, 1u); }  // occupancy of the new state
  }
  workgroupBarrier();
  if (lidx == 0u) { bricks[bi].occupancy = atomicLoad(&occ); }
}
)WGSL";

// Click edit: ray-march to the first solid hit, then carve (mode 1) or spawn
// water (mode 2) a sphere there. One workgroup: thread 0 marches, all carve.
const char* kEditWGSL = R"WGSL(
struct Edit { v0 : vec4<f32>, v1 : vec4<f32> }; // origin.xyz,radius ; dir.xyz,mode
@group(0) @binding(0) var<storage, read_write> voxCur : array<u32>;
@group(0) @binding(1) var<storage, read_write> voxOther : array<u32>;
@group(0) @binding(2) var<storage, read> bricks : array<Brick>;
@group(0) @binding(3) var<uniform> ed : Edit;

fn vIdx(x : i32, y : i32, z : i32) -> u32 {
  let bi = u32((x / 8) + (y / 8) * BG + (z / 8) * BG * BG);
  return bricks[bi].pointer * 512u + u32((x % 8) + (y % 8) * 8 + (z % 8) * 64);
}
fn inW(x : i32, y : i32, z : i32) -> bool { return x >= 0 && y >= 0 && z >= 0 && x < WG && y < WG && z < WG; }

var<workgroup> hit : vec3<i32>;
var<workgroup> pre : vec3<i32>;
var<workgroup> found : u32;

@compute @workgroup_size(64)
fn edit(@builtin(local_invocation_index) lid : u32) {
  if (lid == 0u) {
    found = 0u;
    let ro = ed.v0.xyz;
    let rd = normalize(ed.v1.xyz);
    let inv = vec3<f32>(1.0) / rd;
    let t0 = (vec3<f32>(0.0) - ro) * inv;
    let t1 = (vec3<f32>(f32(WG)) - ro) * inv;
    let tenter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z));
    let texit = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
    if (texit >= max(tenter, 0.0)) {
      let pos = ro + rd * (max(tenter, 0.0) + 0.0001);
      var voxel = vec3<i32>(floor(pos));
      let stp = vec3<i32>(sign(rd));
      let td = abs(inv);
      var tm = (vec3<f32>(f32(voxel.x) + select(0.0, 1.0, stp.x > 0),
                          f32(voxel.y) + select(0.0, 1.0, stp.y > 0),
                          f32(voxel.z) + select(0.0, 1.0, stp.z > 0)) - pos) * inv;
      var prev = voxel;
      for (var i = 0; i < 600; i = i + 1) {
        if (!inW(voxel.x, voxel.y, voxel.z)) { break; }
        if ((voxCur[vIdx(voxel.x, voxel.y, voxel.z)] & 3u) != 0u) { hit = voxel; pre = prev; found = 1u; break; }
        prev = voxel;
        if (tm.x < tm.y && tm.x < tm.z) { voxel.x += stp.x; tm.x += td.x; }
        else if (tm.y < tm.z) { voxel.y += stp.y; tm.y += td.y; }
        else { voxel.z += stp.z; tm.z += td.z; }
      }
    }
  }
  workgroupBarrier();
  if (found == 0u) { return; }
  let mode = u32(ed.v1.w);
  let R = i32(ed.v0.w);
  let center = select(hit, pre, mode == 2u);
  let dim = 2 * R + 1;
  let total = dim * dim * dim;
  for (var idx = i32(lid); idx < total; idx = idx + 64) {
    let lx = idx % dim;
    let ly = (idx / dim) % dim;
    let lz = idx / (dim * dim);
    let dx = lx - R; let dy = ly - R; let dz = lz - R;
    if (dx * dx + dy * dy + dz * dz > R * R) { continue; }
    let wx = center.x + dx; let wy = center.y + dy; let wz = center.z + dz;
    if (!inW(wx, wy, wz)) { continue; }
    let i = vIdx(wx, wy, wz);
    if (mode == 1u) {
      voxCur[i] = 0u; voxOther[i] = 0u; // carve removes static from BOTH buffers
    } else if ((voxCur[i] & 3u) == 0u) {
      let dir = hash3(u32(wx), u32(wy), u32(wz), u32(idx)) & 3u;
      voxCur[i] = (50u << 24u) | (110u << 16u) | (210u << 8u) | 2u | (dir << 2u); // spawn water (current only)
    }
  }
}
)WGSL";

// Concatenate the shared prelude in front of a stage's WGSL.
std::string withCommon(const char* stage)
{
  return std::string(kCommon) + stage;
}
} // namespace

int main(int /*argc*/, char* /*argv*/[])
{
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO) != 0)
  {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  int width = 1280, height = 720;
  SDL_Window* window =
      SDL_CreateWindow("sampleVoxel3D (WebGPU)",
                       SDL_WINDOWPOS_CENTERED,
                       SDL_WINDOWPOS_CENTERED,
                       width,
                       height,
                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!window)
    return 1;

  WGPUInstanceDescriptor instDesc = {};
  WGPUInstance instance = wgpuCreateInstance(&instDesc);
  WGPUSurface surface = SDL_GetWGPUSurface(instance, window);
  WGPUAdapter adapter = requestAdapterSync(instance, surface);
  if (!adapter)
    return 1;
  WGPUDevice device = requestDeviceSync(adapter);
  if (!device)
    return 1;
  WGPUQueue queue = wgpuDeviceGetQueue(device);

  WGPUSurfaceCapabilities caps = {};
  wgpuSurfaceGetCapabilities(surface, adapter, &caps);
  WGPUTextureFormat format =
      caps.formatCount > 0 ? caps.formats[0] : WGPUTextureFormat_BGRA8Unorm;

  // Prefer an unsynced present mode so the FPS counter reflects real GPU
  // throughput (Mailbox = no tearing; else Immediate); fall back to vsync.
  WGPUPresentMode present = WGPUPresentMode_Fifo;
  for (size_t i = 0; i < caps.presentModeCount; ++i)
  {
    if (caps.presentModes[i] == WGPUPresentMode_Mailbox)
    {
      present = WGPUPresentMode_Mailbox;
      break;
    }
    if (caps.presentModes[i] == WGPUPresentMode_Immediate)
      present = WGPUPresentMode_Immediate;
  }

  const auto configure = [&]()
  {
    WGPUSurfaceConfiguration cfg = {};
    cfg.device = device;
    cfg.format = format;
    cfg.usage = WGPUTextureUsage_RenderAttachment;
    cfg.width = static_cast<uint32_t>(width);
    cfg.height = static_cast<uint32_t>(height);
    cfg.presentMode = present;
    cfg.alphaMode = WGPUCompositeAlphaMode_Auto;
    wgpuSurfaceConfigure(surface, &cfg);
  };
  configure();

  const uint64_t brickCount = static_cast<uint64_t>(kBG) * kBG * kBG;
  const uint64_t voxBytes = brickCount * 512 * sizeof(uint32_t);
  const uint64_t brickBytes = brickCount * sizeof(Brick);

  WGPUBuffer voxBuf[2] = {
      makeBuffer(
          device, voxBytes, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst),
      makeBuffer(
          device, voxBytes, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst)};
  WGPUBuffer brickBuf = makeBuffer(device, brickBytes, WGPUBufferUsage_Storage);
  WGPUBuffer camBuf =
      makeBuffer(device, 64, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
  WGPUBuffer frameBuf =
      makeBuffer(device, 16, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
  WGPUBuffer editBuf =
      makeBuffer(device, 32, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);

  // GPU timestamp queries (benchmark HUD): 4 timestamps -- water begin/end,
  // render begin/end -- resolved + read back one frame late.
  const bool hasTs =
      wgpuAdapterHasFeature(adapter, WGPUFeatureName_TimestampQuery);
  WGPUQuerySet tsQuery = nullptr;
  WGPUBuffer tsResolve = nullptr, tsReadback = nullptr;
  if (hasTs)
  {
    WGPUQuerySetDescriptor qd = {};
    qd.type = WGPUQueryType_Timestamp;
    qd.count = 4;
    tsQuery = wgpuDeviceCreateQuerySet(device, &qd);
    tsResolve = makeBuffer(
        device, 32, WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc);
    tsReadback = makeBuffer(
        device, 32, WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead);
  }
  double gpuSimMs = 0.0, gpuRenderMs = 0.0, gpuTotalMs = 0.0;
  bool tsMapBusy = false;
  TsReadback tsRb{tsReadback, &gpuSimMs, &gpuRenderMs, &gpuTotalMs, &tsMapBusy};

  const auto storageEntry =
      [](uint32_t binding, WGPUShaderStage vis, WGPUBufferBindingType type)
  {
    WGPUBindGroupLayoutEntry e = {};
    e.binding = binding;
    e.visibility = vis;
    e.buffer.type = type;
    return e;
  };

  // --- Generator (fills voxBuf[0] + bricks) ---
  WGPUShaderModule genModule = makeShader(device, withCommon(kGenWGSL).c_str());
  WGPUBindGroupLayoutEntry genE[3] = {
      storageEntry(0, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(2, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage)};
  WGPUBindGroupLayoutDescriptor genBglD = {};
  genBglD.entryCount = 3;
  genBglD.entries = genE;
  WGPUBindGroupLayout genBgl =
      wgpuDeviceCreateBindGroupLayout(device, &genBglD);
  WGPUPipelineLayoutDescriptor genPlD = {};
  genPlD.bindGroupLayoutCount = 1;
  genPlD.bindGroupLayouts = &genBgl;
  WGPUPipelineLayout genPl = wgpuDeviceCreatePipelineLayout(device, &genPlD);
  WGPUComputePipelineDescriptor genCpD = {};
  genCpD.layout = genPl;
  genCpD.compute.module = genModule;
  genCpD.compute.entryPoint = sv("generate");
  WGPUComputePipeline genPipeline =
      wgpuDeviceCreateComputePipeline(device, &genCpD);

  WGPUBindGroupEntry genBgE[3] = {};
  genBgE[0].binding = 0;
  genBgE[0].buffer = voxBuf[0];
  genBgE[0].size = voxBytes;
  genBgE[1].binding = 1;
  genBgE[1].buffer = voxBuf[1];
  genBgE[1].size = voxBytes;
  genBgE[2].binding = 2;
  genBgE[2].buffer = brickBuf;
  genBgE[2].size = brickBytes;
  WGPUBindGroupDescriptor genBgD = {};
  genBgD.layout = genBgl;
  genBgD.entryCount = 3;
  genBgD.entries = genBgE;
  WGPUBindGroup genBg = wgpuDeviceCreateBindGroup(device, &genBgD);

  // --- Water sim ---
  WGPUShaderModule waterModule =
      makeShader(device, withCommon(kWaterWGSL).c_str());
  WGPUBindGroupLayoutEntry watE[4] = {
      storageEntry(
          0, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(
          2, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(3, WGPUShaderStage_Compute, WGPUBufferBindingType_Uniform)};
  WGPUBindGroupLayoutDescriptor watBglD = {};
  watBglD.entryCount = 4;
  watBglD.entries = watE;
  WGPUBindGroupLayout watBgl =
      wgpuDeviceCreateBindGroupLayout(device, &watBglD);
  WGPUPipelineLayoutDescriptor watPlD = {};
  watPlD.bindGroupLayoutCount = 1;
  watPlD.bindGroupLayouts = &watBgl;
  WGPUPipelineLayout watPl = wgpuDeviceCreatePipelineLayout(device, &watPlD);
  WGPUComputePipelineDescriptor watCpD = {};
  watCpD.layout = watPl;
  watCpD.compute.module = waterModule;
  watCpD.compute.entryPoint = sv("water");
  WGPUComputePipeline waterPipeline =
      wgpuDeviceCreateComputePipeline(device, &watCpD);

  WGPUBindGroup waterBg[2];
  for (int i = 0; i < 2; ++i)
  {
    WGPUBindGroupEntry e[4] = {};
    e[0].binding = 0;
    e[0].buffer = voxBuf[i];
    e[0].size = voxBytes;
    e[1].binding = 1;
    e[1].buffer = voxBuf[1 - i];
    e[1].size = voxBytes;
    e[2].binding = 2;
    e[2].buffer = brickBuf;
    e[2].size = brickBytes;
    e[3].binding = 3;
    e[3].buffer = frameBuf;
    e[3].size = 16;
    WGPUBindGroupDescriptor d = {};
    d.layout = watBgl;
    d.entryCount = 4;
    d.entries = e;
    waterBg[i] = wgpuDeviceCreateBindGroup(device, &d);
  }

  // --- Occupancy recompute (after each water tick) ---
  WGPUShaderModule recModule =
      makeShader(device, withCommon(kRecountWGSL).c_str());
  WGPUBindGroupLayoutEntry recE[3] = {
      storageEntry(
          0, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(2, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage)};
  WGPUBindGroupLayoutDescriptor recBglD = {};
  recBglD.entryCount = 3;
  recBglD.entries = recE;
  WGPUBindGroupLayout recBgl =
      wgpuDeviceCreateBindGroupLayout(device, &recBglD);
  WGPUPipelineLayoutDescriptor recPlD = {};
  recPlD.bindGroupLayoutCount = 1;
  recPlD.bindGroupLayouts = &recBgl;
  WGPUPipelineLayout recPl = wgpuDeviceCreatePipelineLayout(device, &recPlD);
  WGPUComputePipelineDescriptor recCpD = {};
  recCpD.layout = recPl;
  recCpD.compute.module = recModule;
  recCpD.compute.entryPoint = sv("recount");
  WGPUComputePipeline recPipeline =
      wgpuDeviceCreateComputePipeline(device, &recCpD);
  WGPUBindGroup recBg[2];
  for (int i = 0; i < 2; ++i)
  {
    WGPUBindGroupEntry e[3] = {};
    e[0].binding = 0;
    e[0].buffer = voxBuf[i]; // count the new state
    e[0].size = voxBytes;
    e[1].binding = 1;
    e[1].buffer = voxBuf[1 - i]; // clear dynamic from the old buffer
    e[1].size = voxBytes;
    e[2].binding = 2;
    e[2].buffer = brickBuf;
    e[2].size = brickBytes;
    WGPUBindGroupDescriptor d = {};
    d.layout = recBgl;
    d.entryCount = 3;
    d.entries = e;
    recBg[i] = wgpuDeviceCreateBindGroup(device, &d);
  }

  // --- Click edit (carve / spawn) ---
  WGPUShaderModule editModule =
      makeShader(device, withCommon(kEditWGSL).c_str());
  WGPUBindGroupLayoutEntry edE[4] = {
      storageEntry(0, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(
          2, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(3, WGPUShaderStage_Compute, WGPUBufferBindingType_Uniform)};
  WGPUBindGroupLayoutDescriptor edBglD = {};
  edBglD.entryCount = 4;
  edBglD.entries = edE;
  WGPUBindGroupLayout edBgl = wgpuDeviceCreateBindGroupLayout(device, &edBglD);
  WGPUPipelineLayoutDescriptor edPlD = {};
  edPlD.bindGroupLayoutCount = 1;
  edPlD.bindGroupLayouts = &edBgl;
  WGPUPipelineLayout edPl = wgpuDeviceCreatePipelineLayout(device, &edPlD);
  WGPUComputePipelineDescriptor edCpD = {};
  edCpD.layout = edPl;
  edCpD.compute.module = editModule;
  edCpD.compute.entryPoint = sv("edit");
  WGPUComputePipeline editPipeline =
      wgpuDeviceCreateComputePipeline(device, &edCpD);
  WGPUBindGroup editBg[2];
  for (int i = 0; i < 2; ++i)
  {
    WGPUBindGroupEntry e[4] = {};
    e[0].binding = 0;
    e[0].buffer = voxBuf[i]; // current
    e[0].size = voxBytes;
    e[1].binding = 1;
    e[1].buffer = voxBuf[1 - i]; // other (carve hits both)
    e[1].size = voxBytes;
    e[2].binding = 2;
    e[2].buffer = brickBuf;
    e[2].size = brickBytes;
    e[3].binding = 3;
    e[3].buffer = editBuf;
    e[3].size = 32;
    WGPUBindGroupDescriptor d = {};
    d.layout = edBgl;
    d.entryCount = 4;
    d.entries = e;
    editBg[i] = wgpuDeviceCreateBindGroup(device, &d);
  }

  // --- Render ---
  WGPUShaderModule renderModule =
      makeShader(device, withCommon(kRenderWGSL).c_str());
  WGPUBindGroupLayoutEntry rE[3] = {
      storageEntry(0, WGPUShaderStage_Fragment, WGPUBufferBindingType_Uniform),
      storageEntry(
          1, WGPUShaderStage_Fragment, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          2, WGPUShaderStage_Fragment, WGPUBufferBindingType_ReadOnlyStorage)};
  WGPUBindGroupLayoutDescriptor rBglD = {};
  rBglD.entryCount = 3;
  rBglD.entries = rE;
  WGPUBindGroupLayout rBgl = wgpuDeviceCreateBindGroupLayout(device, &rBglD);
  WGPUPipelineLayoutDescriptor rPlD = {};
  rPlD.bindGroupLayoutCount = 1;
  rPlD.bindGroupLayouts = &rBgl;
  WGPUPipelineLayout rPl = wgpuDeviceCreatePipelineLayout(device, &rPlD);
  WGPUColorTargetState colorTarget = {};
  colorTarget.format = format;
  colorTarget.writeMask = WGPUColorWriteMask_All;
  WGPUFragmentState frag = {};
  frag.module = renderModule;
  frag.entryPoint = sv("fs_main");
  frag.targetCount = 1;
  frag.targets = &colorTarget;
  WGPURenderPipelineDescriptor rpD = {};
  rpD.layout = rPl;
  rpD.vertex.module = renderModule;
  rpD.vertex.entryPoint = sv("vs_main");
  rpD.primitive.topology = WGPUPrimitiveTopology_TriangleList;
  rpD.primitive.frontFace = WGPUFrontFace_CCW;
  rpD.primitive.cullMode = WGPUCullMode_None;
  rpD.multisample.count = 1;
  rpD.multisample.mask = 0xFFFFFFFFu;
  rpD.fragment = &frag;
  WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(device, &rpD);

  WGPUBindGroup renderBg[2];
  for (int i = 0; i < 2; ++i)
  {
    WGPUBindGroupEntry e[3] = {};
    e[0].binding = 0;
    e[0].buffer = camBuf;
    e[0].size = 64;
    e[1].binding = 1;
    e[1].buffer = voxBuf[i];
    e[1].size = voxBytes;
    e[2].binding = 2;
    e[2].buffer = brickBuf;
    e[2].size = brickBytes;
    WGPUBindGroupDescriptor d = {};
    d.layout = rBgl;
    d.entryCount = 3;
    d.entries = e;
    renderBg[i] = wgpuDeviceCreateBindGroup(device, &d);
  }

  // --- Generate the world once ---
  {
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(device, nullptr);
    WGPUComputePassEncoder cp =
        wgpuCommandEncoderBeginComputePass(enc, nullptr);
    wgpuComputePassEncoderSetPipeline(cp, genPipeline);
    wgpuComputePassEncoderSetBindGroup(cp, 0, genBg, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(cp, kBG, kBG, kBG);
    wgpuComputePassEncoderEnd(cp);
    wgpuComputePassEncoderRelease(cp);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
  }

  int srcIdx = 0;
  uint32_t frame = 0;
  int editMode = 0;   // 0 none, 1 carve, 2 spawn water (this frame)
  int heldButton = 0; // mouse button currently held (drives continuous edits)
  float mouseX = 0, mouseY = 0;
  float camYaw = 0.7f;
  float prevTime = static_cast<float>(SDL_GetTicks()) / 1000.0f;
  float fpsAccum = 0.0f;
  int fpsFrames = 0;
  bool running = true;
  while (running)
  {
    SDL_Event e;
    while (SDL_PollEvent(&e))
    {
      if (e.type == SDL_QUIT)
        running = false;
      else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)
        running = false;
      else if (e.type == SDL_MOUSEBUTTONDOWN)
        heldButton = e.button.button == SDL_BUTTON_LEFT
                         ? 1
                         : (e.button.button == SDL_BUTTON_RIGHT ? 2 : 0);
      else if (e.type == SDL_MOUSEBUTTONUP)
        heldButton = 0;
      else if (e.type == SDL_WINDOWEVENT &&
               e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
      {
        width = e.window.data1;
        height = e.window.data2;
        configure();
      }
    }

    // Frame timing + input: Q/E rotate the camera, mouse held carves/spawns.
    const float now = static_cast<float>(SDL_GetTicks()) / 1000.0f;
    const float dt = now - prevTime;
    prevTime = now;
    fpsAccum += dt;
    if (++fpsFrames, fpsAccum >= 0.4f)
    {
      const float fps = static_cast<float>(fpsFrames) / fpsAccum;
      char title[160];
      if (hasTs)
        std::snprintf(
            title,
            sizeof(title),
            "sampleVoxel3D -- %.0f fps (vsync) | GPU %.2f ms = sim %.2f + "
            "render %.2f  [%d^3]",
            fps,
            gpuTotalMs,
            gpuSimMs,
            gpuRenderMs,
            kWorld);
      else
        std::snprintf(title,
                      sizeof(title),
                      "sampleVoxel3D -- %.0f fps  [%d^3]",
                      fps,
                      kWorld);
      SDL_SetWindowTitle(window, title);
      fpsAccum = 0.0f;
      fpsFrames = 0;
    }
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    if (keys[SDL_SCANCODE_Q])
      camYaw -= 1.3f * dt;
    if (keys[SDL_SCANCODE_E])
      camYaw += 1.3f * dt;
    editMode = heldButton;
    if (editMode != 0)
    {
      int mxi, myi;
      SDL_GetMouseState(&mxi, &myi);
      mouseX = static_cast<float>(mxi);
      mouseY = static_cast<float>(myi);
    }

    const Vec3 center = {kWorld * 0.5f, kWorld * 0.2f, kWorld * 0.5f};
    const float radius = kWorld * 0.9f;
    const Vec3 camPos = {center.x + radius * std::cos(camYaw),
                         center.y + kWorld * 0.35f,
                         center.z + radius * std::sin(camYaw)};
    const Vec3 fwd = normalize(center - camPos);
    const Vec3 right = normalize(cross(fwd, Vec3{0, 1, 0}));
    const Vec3 up = cross(right, fwd);
    const float tanHalf = std::tan(0.5f * 1.04719755f);
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const Vec3 rRight = scale(right, tanHalf * aspect);
    const Vec3 rUp = scale(up, tanHalf);
    const float u[16] = {camPos.x,
                         camPos.y,
                         camPos.z,
                         static_cast<float>(kWorld),
                         fwd.x,
                         fwd.y,
                         fwd.z,
                         static_cast<float>(width),
                         rRight.x,
                         rRight.y,
                         rRight.z,
                         static_cast<float>(height),
                         rUp.x,
                         rUp.y,
                         rUp.z,
                         now};
    wgpuQueueWriteBuffer(queue, camBuf, 0, u, sizeof(u));
    const uint32_t fdata[4] = {frame, 0, 0, 0};
    wgpuQueueWriteBuffer(queue, frameBuf, 0, fdata, sizeof(fdata));

    if (editMode != 0)
    {
      const float ndcx = 2.0f * mouseX / static_cast<float>(width) - 1.0f;
      const float ndcy = 1.0f - 2.0f * mouseY / static_cast<float>(height);
      const Vec3 dir = normalize({fwd.x + ndcx * rRight.x + ndcy * rUp.x,
                                  fwd.y + ndcx * rRight.y + ndcy * rUp.y,
                                  fwd.z + ndcx * rRight.z + ndcy * rUp.z});
      const float radius = editMode == 1 ? 12.0f : 8.0f;
      const float ep[8] = {camPos.x,
                           camPos.y,
                           camPos.z,
                           radius,
                           dir.x,
                           dir.y,
                           dir.z,
                           static_cast<float>(editMode)};
      wgpuQueueWriteBuffer(queue, editBuf, 0, ep, sizeof(ep));
    }

    WGPUSurfaceTexture surfTex = {};
    wgpuSurfaceGetCurrentTexture(surface, &surfTex);
    if (surfTex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        surfTex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)
    {
      configure();
      continue;
    }
    WGPUTextureView view = wgpuTextureCreateView(surfTex.texture, nullptr);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(device, nullptr);

    // Click edit (carve/spawn) into the current src, before the water tick.
    if (editMode != 0)
    {
      WGPUComputePassEncoder ce =
          wgpuCommandEncoderBeginComputePass(enc, nullptr);
      wgpuComputePassEncoderSetPipeline(ce, editPipeline);
      wgpuComputePassEncoderSetBindGroup(ce, 0, editBg[srcIdx], 0, nullptr);
      wgpuComputePassEncoderDispatchWorkgroups(ce, 1, 1, 1);
      wgpuComputePassEncoderEnd(ce);
      wgpuComputePassEncoderRelease(ce);
    }

    // Water tick: move src -> dst, then swap. dst's dynamic cells were wiped by
    // last frame's cleanup; its static terrain persists (never cleared/copied).
    const bool doTs = hasTs && !tsMapBusy;
    const int dstIdx = 1 - srcIdx;
    WGPUComputePassTimestampWrites simTsw = {tsQuery, 0, 1};
    WGPUComputePassDescriptor cpd = {};
    if (doTs)
      cpd.timestampWrites = &simTsw;
    WGPUComputePassEncoder cp = wgpuCommandEncoderBeginComputePass(enc, &cpd);
    wgpuComputePassEncoderSetPipeline(cp, waterPipeline);
    wgpuComputePassEncoderSetBindGroup(cp, 0, waterBg[srcIdx], 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(cp, kBG, kBG, kBG);
    wgpuComputePassEncoderEnd(cp);
    wgpuComputePassEncoderRelease(cp);
    srcIdx = dstIdx;

    // Recompute brick occupancy on the new state so brick-skip stays correct
    // as water moves between bricks.
    WGPUComputePassEncoder cr =
        wgpuCommandEncoderBeginComputePass(enc, nullptr);
    wgpuComputePassEncoderSetPipeline(cr, recPipeline);
    wgpuComputePassEncoderSetBindGroup(cr, 0, recBg[srcIdx], 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(cr, kBG, kBG, kBG);
    wgpuComputePassEncoderEnd(cr);
    wgpuComputePassEncoderRelease(cr);

    WGPURenderPassColorAttachment color = {};
    color.view = view;
    color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = WGPUColor{0.02, 0.02, 0.04, 1.0};
    WGPURenderPassTimestampWrites renderTsw = {tsQuery, 2, 3};
    WGPURenderPassDescriptor pass = {};
    pass.colorAttachmentCount = 1;
    pass.colorAttachments = &color;
    if (doTs)
      pass.timestampWrites = &renderTsw;
    WGPURenderPassEncoder rp = wgpuCommandEncoderBeginRenderPass(enc, &pass);
    wgpuRenderPassEncoderSetPipeline(rp, pipeline);
    wgpuRenderPassEncoderSetBindGroup(rp, 0, renderBg[srcIdx], 0, nullptr);
    wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(rp);
    wgpuRenderPassEncoderRelease(rp);

    if (doTs)
    {
      wgpuCommandEncoderResolveQuerySet(enc, tsQuery, 0, 4, tsResolve, 0);
      wgpuCommandEncoderCopyBufferToBuffer(
          enc, tsResolve, 0, tsReadback, 0, 32);
    }

    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(queue, 1, &cmd);
    wgpuSurfacePresent(surface);

    if (doTs)
    {
      tsMapBusy = true;
      WGPUBufferMapCallbackInfo mci = {};
      mci.mode = WGPUCallbackMode_AllowProcessEvents;
      mci.callback = onTsMapped;
      mci.userdata1 = &tsRb;
      wgpuBufferMapAsync(tsReadback, WGPUMapMode_Read, 0, 32, mci);
    }
    if (hasTs)
      wgpuDevicePoll(
          device, 0, nullptr); // fire the previous frame's readback callback

    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
    wgpuTextureViewRelease(view);
    wgpuTextureRelease(surfTex.texture);
    ++frame;
  }

  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
