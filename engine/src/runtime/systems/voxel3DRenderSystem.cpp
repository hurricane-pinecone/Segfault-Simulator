#include "engine/runtime/systems/voxel3DRenderSystem.h"

#include "engine/core/logger/logger.h"
#include "engine/core/util/parallelFor.h"
#include "engine/core/util/profiling.h"
#include "engine/core/voxel/tinyVoxelMesher.h"
#include "engine/generated/embeddedShaders.h"
#include "engine/runtime/voxel/tinyVoxelWorld.h"
#include "engine/runtime/voxel/voxelFireSystem.h"
#include "engine/runtime/voxel/waterSurfaceSystem.h"
#include "glm/glm/common.hpp"
#include "glm/glm/ext/matrix_transform.hpp" // rotate
#include "glm/glm/geometric.hpp"
#include "glm/glm/gtc/type_ptr.hpp"
#include "glm/glm/matrix.hpp" // inverse
#include "glm/glm/trigonometric.hpp"

#include <algorithm>

#ifdef __EMSCRIPTEN__
  #include <GLES3/gl3.h>
#else
  #include <GL/glew.h>
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sfs
{
namespace
{

unsigned int compileShader(unsigned int type, const char* source)
{
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);

  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok)
  {
    char log[1024];
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    LOG_ERROR(log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

unsigned int buildProgram()
{
#ifdef __EMSCRIPTEN__
  const std::string version = "#version 300 es\nprecision mediump float;\n";
#else
  const std::string version = "#version 330 core\n";
#endif

  const std::string vs = version + std::string(sfs::shaders::voxel3dVert);
  const std::string fs = version + std::string(sfs::shaders::voxel3dFrag);

  GLuint v = compileShader(GL_VERTEX_SHADER, vs.c_str());
  GLuint f = compileShader(GL_FRAGMENT_SHADER, fs.c_str());
  if (v == 0 || f == 0)
  {
    if (v)
      glDeleteShader(v);
    if (f)
      glDeleteShader(f);
    return 0;
  }

  GLuint program = glCreateProgram();
  glAttachShader(program, v);
  glAttachShader(program, f);
  glLinkProgram(program);
  glDeleteShader(v);
  glDeleteShader(f);

  GLint ok = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok)
  {
    char log[1024];
    glGetProgramInfoLog(program, sizeof(log), nullptr, log);
    LOG_ERROR(log);
    glDeleteProgram(program);
    return 0;
  }
  return program;
}

// Bind the standard tiny-voxel vertex layout (pos / normal / colour) on the
// currently-bound VAO + VBO.
void setupAttribs()
{
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(
      0,
      3,
      GL_FLOAT,
      GL_FALSE,
      sizeof(TinyVoxelVertex),
      reinterpret_cast<void*>(offsetof(TinyVoxelVertex, pos)));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(
      1,
      3,
      GL_FLOAT,
      GL_FALSE,
      sizeof(TinyVoxelVertex),
      reinterpret_cast<void*>(offsetof(TinyVoxelVertex, normal)));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(
      2,
      4,
      GL_FLOAT,
      GL_FALSE,
      sizeof(TinyVoxelVertex),
      reinterpret_cast<void*>(offsetof(TinyVoxelVertex, color)));
}

// A standalone solid box (all 6 faces), for the player marker.
std::vector<TinyVoxelVertex>
buildBox(const glm::vec3& center, const glm::vec3& half, const glm::vec3& color)
{
  const glm::vec3 lo = center - half;
  const glm::vec3 hi = center + half;
  const glm::vec3 c[8] = {{lo.x, lo.y, lo.z},
                          {hi.x, lo.y, lo.z},
                          {hi.x, hi.y, lo.z},
                          {lo.x, hi.y, lo.z},
                          {lo.x, lo.y, hi.z},
                          {hi.x, lo.y, hi.z},
                          {hi.x, hi.y, hi.z},
                          {lo.x, hi.y, hi.z}};

  std::vector<TinyVoxelVertex> out;
  const glm::vec4 c4{color, 1.0f}; // opaque
  const auto quad = [&](int a, int b, int d, int e, const glm::vec3& n)
  {
    out.push_back({c[a], n, c4});
    out.push_back({c[b], n, c4});
    out.push_back({c[d], n, c4});
    out.push_back({c[a], n, c4});
    out.push_back({c[d], n, c4});
    out.push_back({c[e], n, c4});
  };
  quad(1, 2, 6, 5, {1, 0, 0});  // +x
  quad(0, 4, 7, 3, {-1, 0, 0}); // -x
  quad(3, 7, 6, 2, {0, 1, 0});  // +y
  quad(0, 1, 5, 4, {0, -1, 0}); // -y
  quad(4, 5, 6, 7, {0, 0, 1});  // +z
  quad(0, 3, 2, 1, {0, 0, -1}); // -z
  return out;
}

// Build a cube mesh for every live debris voxel (faded by remaining life).
std::vector<TinyVoxelVertex>
buildDebrisMesh(const std::vector<Voxel3DRenderSystem::Debris>& debris,
                float half)
{
  std::vector<TinyVoxelVertex> out;
  out.reserve(debris.size() * 36);
  for (const auto& d : debris)
  {
    const glm::vec3 lo = d.pos - glm::vec3(half);
    const glm::vec3 hi = d.pos + glm::vec3(half);
    const glm::vec3 c[8] = {{lo.x, lo.y, lo.z},
                            {hi.x, lo.y, lo.z},
                            {hi.x, hi.y, lo.z},
                            {lo.x, hi.y, lo.z},
                            {lo.x, lo.y, hi.z},
                            {hi.x, lo.y, hi.z},
                            {hi.x, hi.y, hi.z},
                            {lo.x, hi.y, hi.z}};
    const glm::vec4 col =
        d.color; // opaque -- debris is real voxels, not a puff
    const auto quad = [&](int a, int b, int e, int f, const glm::vec3& n)
    {
      out.push_back({c[a], n, col});
      out.push_back({c[b], n, col});
      out.push_back({c[e], n, col});
      out.push_back({c[a], n, col});
      out.push_back({c[e], n, col});
      out.push_back({c[f], n, col});
    };
    quad(1, 2, 6, 5, {1, 0, 0});
    quad(0, 4, 7, 3, {-1, 0, 0});
    quad(3, 7, 6, 2, {0, 1, 0});
    quad(0, 1, 5, 4, {0, -1, 0});
    quad(4, 5, 6, 7, {0, 0, 1});
    quad(0, 3, 2, 1, {0, 0, -1});
  }
  return out;
}

// Build a cube for each flame puff: shrinks + fades + cools (yellow -> red)
// with age. Drawn emissive + additive, so the cubes read as glowing fire.
std::vector<TinyVoxelVertex>
buildFlameMesh(const std::vector<Voxel3DRenderSystem::Flame>& flames)
{
  std::vector<TinyVoxelVertex> out;
  out.reserve(flames.size() * 36);
  const glm::vec3 hot{1.0f, 0.85f, 0.30f};   // young: bright yellow-orange
  const glm::vec3 cool{0.85f, 0.15f, 0.05f}; // old: deep red
  const glm::vec3 nrm{0.0f, 1.0f, 0.0f}; // unused (emissive ignores normals)
  for (const auto& f : flames)
  {
    const float t =
        glm::clamp(f.life / glm::max(f.maxLife, 0.001f), 0.0f, 1.0f); // 1 young
    const float half = 0.6f + 1.6f * t;
    const glm::vec4 col{glm::mix(cool, hot, t), t * 0.9f};
    const glm::vec3 lo = f.pos - glm::vec3(half);
    const glm::vec3 hi = f.pos + glm::vec3(half);
    const glm::vec3 c[8] = {{lo.x, lo.y, lo.z},
                            {hi.x, lo.y, lo.z},
                            {hi.x, hi.y, lo.z},
                            {lo.x, hi.y, lo.z},
                            {lo.x, lo.y, hi.z},
                            {hi.x, lo.y, hi.z},
                            {hi.x, hi.y, hi.z},
                            {lo.x, hi.y, hi.z}};
    const auto quad = [&](int a, int b, int e, int g)
    {
      out.push_back({c[a], nrm, col});
      out.push_back({c[b], nrm, col});
      out.push_back({c[e], nrm, col});
      out.push_back({c[a], nrm, col});
      out.push_back({c[e], nrm, col});
      out.push_back({c[g], nrm, col});
    };
    quad(1, 2, 6, 5);
    quad(0, 4, 7, 3);
    quad(3, 7, 6, 2);
    quad(0, 1, 5, 4);
    quad(4, 5, 6, 7);
    quad(0, 3, 2, 1);
  }
  return out;
}

// Build a small bright cube for each airborne ember (drawn emissive +
// additive).
std::vector<TinyVoxelVertex>
buildEmberMesh(const std::vector<VoxelFireSystem::Ember>& embers)
{
  std::vector<TinyVoxelVertex> out;
  out.reserve(embers.size() * 36);
  const glm::vec3 nrm{0.0f, 1.0f, 0.0f};
  const glm::vec4 col{1.0f, 0.72f, 0.32f, 0.95f}; // hot spark
  const float half = 0.7f;
  for (const auto& e : embers)
  {
    const glm::vec3 lo = e.pos - glm::vec3(half);
    const glm::vec3 hi = e.pos + glm::vec3(half);
    const glm::vec3 c[8] = {{lo.x, lo.y, lo.z},
                            {hi.x, lo.y, lo.z},
                            {hi.x, hi.y, lo.z},
                            {lo.x, hi.y, lo.z},
                            {lo.x, lo.y, hi.z},
                            {hi.x, lo.y, hi.z},
                            {hi.x, hi.y, hi.z},
                            {lo.x, hi.y, hi.z}};
    const auto quad = [&](int a, int b, int ei, int g)
    {
      out.push_back({c[a], nrm, col});
      out.push_back({c[b], nrm, col});
      out.push_back({c[ei], nrm, col});
      out.push_back({c[a], nrm, col});
      out.push_back({c[ei], nrm, col});
      out.push_back({c[g], nrm, col});
    };
    quad(1, 2, 6, 5);
    quad(0, 4, 7, 3);
    quad(3, 7, 6, 2);
    quad(0, 1, 5, 4);
    quad(4, 5, 6, 7);
    quad(0, 3, 2, 1);
  }
  return out;
}

// Build ONE 32-col water tile as a voxel-quantized translucent sheet: a top
// quad at each column's surface Y, plus vertical step faces where a neighbour's
// water (or bare bed) sits lower. Depth tints shallow->deep + raises opacity.
// Tiling means only the columns that changed re-mesh -- water never re-meshes
// terrain.
std::vector<TinyVoxelVertex> buildWaterTileMesh(const WaterSurfaceSystem& water,
                                                const TinyVoxelWorld& world,
                                                int tileX,
                                                int tileZ)
{
  std::vector<TinyVoxelVertex> out;
  const int x0 = tileX * 32;
  const int z0 = tileZ * 32;
  const glm::vec3 shallow{0.42f, 0.70f, 0.86f};
  const glm::vec3 deep{0.10f, 0.28f, 0.56f};

  const auto surfOf = [&](int x, int z) -> int
  {
    return water.hasWater(x, z)
               ? static_cast<int>(glm::round(water.surfaceAt(x, z)))
               : world.surfaceTop(x, z); // bare bed
  };

  for (int lz = 0; lz < 32; ++lz)
    for (int lx = 0; lx < 32; ++lx)
    {
      const int x = x0 + lx;
      const int z = z0 + lz;
      if (!water.hasWater(x, z))
        continue;
      const int surf = static_cast<int>(glm::round(water.surfaceAt(x, z)));
      const int bed = world.surfaceTop(x, z);
      if (surf <= bed)
        continue;
      const float d01 =
          glm::clamp(static_cast<float>(surf - bed) / 96.0f, 0.0f, 1.0f);
      const glm::vec4 col{glm::mix(shallow, deep, d01),
                          glm::clamp(0.40f + d01 * 0.42f, 0.40f, 0.85f)};
      const float fx = static_cast<float>(x);
      const float fz = static_cast<float>(z);
      const float fy = static_cast<float>(surf);

      const glm::vec3 up{0.0f, 1.0f, 0.0f};
      out.push_back({{fx, fy, fz}, up, col});
      out.push_back({{fx, fy, fz + 1}, up, col});
      out.push_back({{fx + 1, fy, fz + 1}, up, col});
      out.push_back({{fx, fy, fz}, up, col});
      out.push_back({{fx + 1, fy, fz + 1}, up, col});
      out.push_back({{fx + 1, fy, fz}, up, col});

      // Step faces down to lower neighbours (winding is loose -- cull is off).
      const auto side = [&](int nx,
                            int nz,
                            const glm::vec3& nrm,
                            const glm::vec3& a,
                            const glm::vec3& b)
      {
        const int lowY = glm::max(surfOf(nx, nz), bed);
        if (lowY >= surf)
          return;
        const float ly = static_cast<float>(lowY);
        out.push_back({{a.x, ly, a.z}, nrm, col});
        out.push_back({{b.x, ly, b.z}, nrm, col});
        out.push_back({{b.x, fy, b.z}, nrm, col});
        out.push_back({{a.x, ly, a.z}, nrm, col});
        out.push_back({{b.x, fy, b.z}, nrm, col});
        out.push_back({{a.x, fy, a.z}, nrm, col});
      };
      side(x + 1, z, {1, 0, 0}, {fx + 1, 0, fz}, {fx + 1, 0, fz + 1});
      side(x - 1, z, {-1, 0, 0}, {fx, 0, fz + 1}, {fx, 0, fz});
      side(x, z + 1, {0, 0, 1}, {fx + 1, 0, fz + 1}, {fx, 0, fz + 1});
      side(x, z - 1, {0, 0, -1}, {fx, 0, fz}, {fx + 1, 0, fz});
    }
  return out;
}

// Small translucent cubes for flying water droplets (splash spray +
// waterfalls).
std::vector<TinyVoxelVertex>
buildDropletMesh(const std::vector<Voxel3DRenderSystem::Droplet>& drops)
{
  std::vector<TinyVoxelVertex> out;
  out.reserve(drops.size() * 36);
  const glm::vec3 nrm{0.0f, 1.0f, 0.0f};
  const glm::vec4 col{0.55f, 0.78f, 0.95f, 0.75f};
  const float half = 0.9f;
  for (const auto& d : drops)
  {
    const glm::vec3 lo = d.pos - glm::vec3(half);
    const glm::vec3 hi = d.pos + glm::vec3(half);
    const glm::vec3 c[8] = {{lo.x, lo.y, lo.z},
                            {hi.x, lo.y, lo.z},
                            {hi.x, hi.y, lo.z},
                            {lo.x, hi.y, lo.z},
                            {lo.x, lo.y, hi.z},
                            {hi.x, lo.y, hi.z},
                            {hi.x, hi.y, hi.z},
                            {lo.x, hi.y, hi.z}};
    const auto quad = [&](int a, int b, int ei, int g)
    {
      out.push_back({c[a], nrm, col});
      out.push_back({c[b], nrm, col});
      out.push_back({c[ei], nrm, col});
      out.push_back({c[a], nrm, col});
      out.push_back({c[ei], nrm, col});
      out.push_back({c[g], nrm, col});
    };
    quad(1, 2, 6, 5);
    quad(0, 4, 7, 3);
    quad(3, 7, 6, 2);
    quad(0, 1, 5, 4);
    quad(4, 5, 6, 7);
    quad(0, 3, 2, 1);
  }
  return out;
}

// Build ONE block's mesh in LOCAL space (centred on the block, no rotation, no
// translation) -- built once at spawn. The block tumbles + moves on the GPU via
// the per-block uModel = translate(pos) * rotate(angle).
std::vector<TinyVoxelVertex>
buildBlockLocalMesh(const Voxel3DRenderSystem::Block& b)
{
  std::vector<TinyVoxelVertex> out;
  const float c = static_cast<float>(b.size) * 0.5f;
  for (int lz = 0; lz < b.size; ++lz)
    for (int ly = 0; ly < b.size; ++ly)
      for (int lx = 0; lx < b.size; ++lx)
      {
        const std::uint32_t col = b.voxels[static_cast<std::size_t>(
            (lz * b.size + ly) * b.size + lx)];
        if (col == 0u)
          continue;
        const glm::vec3 lc{static_cast<float>(lx) + 0.5f - c,
                           static_cast<float>(ly) + 0.5f - c,
                           static_cast<float>(lz) + 0.5f - c};
        glm::vec3 corner[8];
        for (int i = 0; i < 8; ++i)
          corner[i] = lc + glm::vec3{(i & 1) ? 0.5f : -0.5f,
                                     (i & 2) ? 0.5f : -0.5f,
                                     (i & 4) ? 0.5f : -0.5f};
        const glm::vec4 color{static_cast<float>((col >> 24) & 0xFFu) / 255.0f,
                              static_cast<float>((col >> 16) & 0xFFu) / 255.0f,
                              static_cast<float>((col >> 8) & 0xFFu) / 255.0f,
                              1.0f};
        const auto face = [&](int a, int bb, int e, int f, const glm::vec3& n)
        {
          out.push_back({corner[a], n, color});
          out.push_back({corner[bb], n, color});
          out.push_back({corner[e], n, color});
          out.push_back({corner[a], n, color});
          out.push_back({corner[e], n, color});
          out.push_back({corner[f], n, color});
        };
        face(1, 3, 7, 5, {1, 0, 0});
        face(0, 4, 6, 2, {-1, 0, 0});
        face(2, 3, 7, 6, {0, 1, 0});
        face(0, 1, 5, 4, {0, -1, 0});
        face(4, 5, 7, 6, {0, 0, 1});
        face(0, 2, 3, 1, {0, 0, -1});
      }
  return out;
}

// An immutable copy of everything meshing a chunk needs: its voxels plus the
// render-CLASS (air/water/opaque) of each neighbour's bordering layer. Captured
// on the main thread so a worker can mesh with zero access to the mutating
// world.
struct MeshSnapshot
{
  glm::ivec3 coord{0};
  std::uint64_t gen = 0;
  TinyVoxelChunk chunk;
  std::array<std::array<std::uint8_t, kTinyChunkSize * kTinyChunkSize>, 6>
      border{};
};

MeshSnapshot buildSnapshot(const TinyVoxelWorld& world,
                           const glm::ivec3& coord,
                           std::uint64_t gen)
{
  constexpr int N = kTinyChunkSize;
  MeshSnapshot s;
  s.coord = coord;
  s.gen = gen;
  s.chunk = world.chunks().at(coord); // exists: caller checked

  const TinyVoxelWorld::ChunkMap& chunks = world.chunks();
  const auto find = [&](const glm::ivec3& c) -> const TinyVoxelChunk*
  {
    const auto it = chunks.find(c);
    return it == chunks.end() ? nullptr : &it->second;
  };
  const auto cls = [](const TinyVoxelChunk* c, int x, int y, int z)
  {
    return c ? static_cast<std::uint8_t>(tinyClassOf(c->at(x, y, z)))
             : static_cast<std::uint8_t>(TinyClass::Air);
  };
  const TinyVoxelChunk* nx = find(coord + glm::ivec3{-1, 0, 0});
  const TinyVoxelChunk* px = find(coord + glm::ivec3{1, 0, 0});
  const TinyVoxelChunk* ny = find(coord + glm::ivec3{0, -1, 0});
  const TinyVoxelChunk* py = find(coord + glm::ivec3{0, 1, 0});
  const TinyVoxelChunk* nz = find(coord + glm::ivec3{0, 0, -1});
  const TinyVoxelChunk* pz = find(coord + glm::ivec3{0, 0, 1});
  for (int a = 0; a < N; ++a)
    for (int b = 0; b < N; ++b)
    {
      const std::size_t i = static_cast<std::size_t>(a * N + b);
      s.border[0][i] = cls(nx, N - 1, a, b); // -x neighbour's +x layer
      s.border[1][i] = cls(px, 0, a, b);     // +x neighbour's -x layer
      s.border[2][i] = cls(ny, a, N - 1, b); // -y
      s.border[3][i] = cls(py, a, 0, b);     // +y
      s.border[4][i] = cls(nz, a, b, N - 1); // -z
      s.border[5][i] = cls(pz, a, b, 0);     // +z
    }
  return s;
}

TinyChunkMesh meshSnapshot(const MeshSnapshot& s)
{
  constexpr int N = kTinyChunkSize;
  const glm::ivec3 origin = s.coord * N;
  // Only queried for cells OUTSIDE the chunk (the mesher reads in-chunk
  // directly).
  const auto classOutside = [&](int x, int y, int z) -> int
  {
    const int lx = x - origin.x;
    const int ly = y - origin.y;
    const int lz = z - origin.z;
    if (lx == -1)
      return s.border[0][static_cast<std::size_t>(ly * N + lz)];
    if (lx == N)
      return s.border[1][static_cast<std::size_t>(ly * N + lz)];
    if (ly == -1)
      return s.border[2][static_cast<std::size_t>(lx * N + lz)];
    if (ly == N)
      return s.border[3][static_cast<std::size_t>(lx * N + lz)];
    if (lz == -1)
      return s.border[4][static_cast<std::size_t>(lx * N + ly)];
    if (lz == N)
      return s.border[5][static_cast<std::size_t>(lx * N + ly)];
    return static_cast<int>(TinyClass::Air);
  };
  return meshTinyChunk(origin, s.chunk, classOutside);
}

void uploadMesh(Voxel3DRenderSystem::GpuMesh& mesh,
                const std::vector<TinyVoxelVertex>& verts)
{
  if (mesh.vao == 0)
    glGenVertexArrays(1, &mesh.vao);
  if (mesh.vbo == 0)
    glGenBuffers(1, &mesh.vbo);
  glBindVertexArray(mesh.vao);
  glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(verts.size() * sizeof(TinyVoxelVertex)),
               verts.data(),
               GL_STATIC_DRAW);
  setupAttribs();
  mesh.vertexCount = static_cast<int>(verts.size());
}

} // namespace

Voxel3DRenderSystem::Voxel3DRenderSystem(int windowWidth, int windowHeight)
    : m_viewportW(windowWidth > 0 ? windowWidth : 1),
      m_viewportH(windowHeight > 0 ? windowHeight : 1), m_meshQueue(2)
{
}

Voxel3DRenderSystem::~Voxel3DRenderSystem()
{
  for (auto& [coord, cg] : m_gpu)
    for (GpuMesh* m : {&cg.opaque, &cg.water})
    {
      if (m->vbo)
        glDeleteBuffers(1, &m->vbo);
      if (m->vao)
        glDeleteVertexArrays(1, &m->vao);
    }
  for (auto& [t, mesh] : m_waterTiles)
  {
    if (mesh.vbo)
      glDeleteBuffers(1, &mesh.vbo);
    if (mesh.vao)
      glDeleteVertexArrays(1, &mesh.vao);
  }
  for (GpuMesh* m : {&m_playerMesh,
                     &m_debrisMesh,
                     &m_flameMesh,
                     &m_emberMesh,
                     &m_dropletMesh})
  {
    if (m->vbo)
      glDeleteBuffers(1, &m->vbo);
    if (m->vao)
      glDeleteVertexArrays(1, &m->vao);
  }
  for (Block& b : m_blocks)
  {
    if (b.mesh.vbo)
      glDeleteBuffers(1, &b.mesh.vbo);
    if (b.mesh.vao)
      glDeleteVertexArrays(1, &b.mesh.vao);
  }
  if (m_program)
    glDeleteProgram(m_program);
}

void Voxel3DRenderSystem::setViewport(int width, int height)
{
  m_viewportW = width > 0 ? width : 1;
  m_viewportH = height > 0 ? height : 1;
}

bool Voxel3DRenderSystem::raycastVoxel(const glm::vec2& ndc,
                                       glm::ivec3& outVoxel) const
{
  if (!m_world)
    return false;
  const glm::mat4 inv = glm::inverse(m_camera.viewProj());
  glm::vec4 a = inv * glm::vec4(ndc.x, ndc.y, -1.0f, 1.0f);
  glm::vec4 b = inv * glm::vec4(ndc.x, ndc.y, 1.0f, 1.0f);
  const glm::vec3 origin = glm::vec3(a) / a.w;
  const glm::vec3 dir = glm::normalize(glm::vec3(b) / b.w - origin);

  glm::vec3 p = origin;
  for (int i = 0; i < 5000; ++i)
  {
    const int vx = static_cast<int>(glm::floor(p.x));
    const int vy = static_cast<int>(glm::floor(p.y));
    const int vz = static_cast<int>(glm::floor(p.z));
    if (m_world->solidAt(vx, vy, vz))
    {
      outVoxel = glm::ivec3{vx, vy, vz};
      return true;
    }
    p += dir * 0.5f;
  }
  return false;
}

void Voxel3DRenderSystem::explode(const glm::ivec3& center,
                                  int radius,
                                  float power)
{
  ZoneScopedN("Voxel3D::explode");
  if (!m_world)
    return;
  constexpr std::size_t kMaxDebris = 4000;
  constexpr std::size_t kMaxBlocks = 400;
  const int r2 = radius * radius;
  const glm::vec3 c{static_cast<float>(center.x) + 0.5f,
                    static_cast<float>(center.y) + 0.5f,
                    static_cast<float>(center.z) + 0.5f};

  // Wake the surrounding water so it FLOWS into the cavity the carve opens.
  if (m_waterSurf)
    m_waterSurf->wake(center, radius + 4);

  // Splash: a blast at/under water flings spray droplets up + outward.
  if (m_waterSurf && m_waterSurf->hasWater(center.x, center.z))
  {
    const float surf = m_waterSurf->surfaceAt(center.x, center.z);
    const int n = glm::min(radius * 2, 90);
    for (int i = 0; i < n; ++i)
    {
      std::uint32_t h = static_cast<std::uint32_t>(center.x) * 92837111u ^
                        static_cast<std::uint32_t>(center.z) * 689287499u ^
                        static_cast<std::uint32_t>(i) * 283923481u;
      h = (h ^ (h >> 13)) * 1274126177u;
      const float a = static_cast<float>(h & 0xFFFFu) / 65535.0f * 6.2832f;
      const float spd = 18.0f + static_cast<float>((h >> 16) & 0x3Fu);
      const float up = 55.0f + static_cast<float>((h >> 22) & 0x3Fu);
      m_droplets.push_back({{c.x, surf + 1.0f, c.z},
                            {glm::cos(a) * spd, up, glm::sin(a) * spd},
                            0.0f});
    }
  }

  // First, grab sizable solid cubes from inside the sphere as rigid blocks
  // (they tumble + roll), carving them out so the spray loop below skips them.
  const int step = glm::max(4, radius / 3);
  for (int bz = -radius; bz <= radius; bz += step)
    for (int by = -radius; by <= radius; by += step)
      for (int bx = -radius; bx <= radius; bx += step)
      {
        if (bx * bx + by * by + bz * bz > r2 || m_blocks.size() >= kMaxBlocks)
          continue;
        const glm::ivec3 bcenter{center.x + bx, center.y + by, center.z + bz};
        std::uint32_t hb = static_cast<std::uint32_t>(bcenter.x) * 73856093u ^
                           static_cast<std::uint32_t>(bcenter.y) * 19349663u ^
                           static_cast<std::uint32_t>(bcenter.z) * 83492791u;
        hb = (hb ^ (hb >> 13)) * 1274126177u;
        const int size = 2 + static_cast<int>((hb & 3u)); // 2..4
        const int hh = size / 2;

        Block blk;
        blk.size = size;
        blk.voxels.assign(static_cast<std::size_t>(size * size * size), 0u);
        int filled = 0;
        for (int lz = 0; lz < size; ++lz)
          for (int ly = 0; ly < size; ++ly)
            for (int lx = 0; lx < size; ++lx)
            {
              const int wx = bcenter.x - hh + lx;
              const int wy = bcenter.y - hh + ly;
              const int wz = bcenter.z - hh + lz;
              const std::uint32_t col = m_world->voxelAt(wx, wy, wz);
              if (col == 0u)
                continue;
              blk.voxels[static_cast<std::size_t>((lz * size + ly) * size +
                                                  lx)] = col;
              m_world->setVoxel(wx, wy, wz, 0u); // carve
              ++filled;
            }
        if (filled < 3)
          continue; // too thin to bother

        blk.pos = glm::vec3{static_cast<float>(bcenter.x) + 0.5f,
                            static_cast<float>(bcenter.y) + 0.5f,
                            static_cast<float>(bcenter.z) + 0.5f};
        glm::vec3 outDir = blk.pos - c;
        const float len = glm::length(outDir);
        outDir = len > 0.001f ? outDir / len : glm::vec3{0.0f, 1.0f, 0.0f};
        const float rnd = 0.5f + static_cast<float>((hb >> 5) & 0xFFu) / 255.0f;
        blk.vel = outDir * (power * rnd) + glm::vec3{0.0f, power * 0.35f, 0.0f};
        const auto spin = [&](int s) {
          return (static_cast<float>((hb >> s) & 0xFFu) / 255.0f - 0.5f) *
                 14.0f;
        };
        blk.angVel = glm::vec3{spin(2), spin(10), spin(18)};
        m_blocks.push_back(std::move(blk));
      }

  for (int dz = -radius; dz <= radius; ++dz)
    for (int dy = -radius; dy <= radius; ++dy)
      for (int dx = -radius; dx <= radius; ++dx)
      {
        if (dx * dx + dy * dy + dz * dz > r2)
          continue;
        const int wx = center.x + dx;
        const int wy = center.y + dy;
        const int wz = center.z + dz;
        const std::uint32_t col = m_world->voxelAt(wx, wy, wz);
        if (col == 0u)
          continue;                        // already air
        m_world->setVoxel(wx, wy, wz, 0u); // carve

        std::uint32_t h = static_cast<std::uint32_t>(wx) * 73856093u ^
                          static_cast<std::uint32_t>(wy) * 19349663u ^
                          static_cast<std::uint32_t>(wz) * 83492791u;
        h = (h ^ (h >> 13)) * 1274126177u;
        if (m_debris.size() < kMaxDebris && (h & 7u) == 0u)
        {
          glm::vec3 outDir{static_cast<float>(wx) + 0.5f - c.x,
                           static_cast<float>(wy) + 0.5f - c.y,
                           static_cast<float>(wz) + 0.5f - c.z};
          const float len = glm::length(outDir);
          outDir = len > 0.001f ? outDir / len : glm::vec3{0.0f, 1.0f, 0.0f};
          const float rnd =
              0.5f + static_cast<float>((h >> 3) & 0xFFu) / 255.0f;
          const glm::vec3 vel =
              outDir * (power * rnd) + glm::vec3{0.0f, power * 0.4f, 0.0f};
          const glm::vec4 dcol{static_cast<float>((col >> 24) & 0xFFu) / 255.0f,
                               static_cast<float>((col >> 16) & 0xFFu) / 255.0f,
                               static_cast<float>((col >> 8) & 0xFFu) / 255.0f,
                               1.0f};
          // life is just a safety net now -- debris persists until it lands.
          m_debris.push_back({{static_cast<float>(wx) + 0.5f,
                               static_cast<float>(wy) + 0.5f,
                               static_cast<float>(wz) + 0.5f},
                              vel,
                              dcol,
                              12.0f});
        }
      }
}

void Voxel3DRenderSystem::ensureInitialized()
{
  if (m_initialized)
    return;
  m_initialized = true;

  m_program = buildProgram();
  if (m_program == 0)
  {
    LOG_ERROR("Voxel3DRenderSystem: shader program failed to build");
    return;
  }
  m_uViewProj = glGetUniformLocation(m_program, "uViewProj");
  m_uModel = glGetUniformLocation(m_program, "uModel");
  m_uEmissive = glGetUniformLocation(m_program, "uEmissive");
  m_uLightDir = glGetUniformLocation(m_program, "uLightDir");
  m_uSunColor = glGetUniformLocation(m_program, "uSunColor");
  m_uAmbient = glGetUniformLocation(m_program, "uAmbient");
  m_uSunDiffuse = glGetUniformLocation(m_program, "uSunDiffuse");
  m_uLightCount = glGetUniformLocation(m_program, "uLightCount");
  m_uLightPos = glGetUniformLocation(m_program, "uLightPos");
  m_uLightColor = glGetUniformLocation(m_program, "uLightColor");
  m_uLightRadius = glGetUniformLocation(m_program, "uLightRadius");
  m_uLightIntensity = glGetUniformLocation(m_program, "uLightIntensity");
}

void Voxel3DRenderSystem::update(double deltaTime)
{
  ZoneScopedN("Voxel3D::update");
  if (!m_world)
    return;

  const float dt = static_cast<float>(deltaTime);
  const float gravity = 170.0f;
  const auto voxelOf = [](const glm::vec3& p)
  {
    return glm::ivec3{static_cast<int>(glm::floor(p.x)),
                      static_cast<int>(glm::floor(p.y)),
                      static_cast<int>(glm::floor(p.z))};
  };

  // Splash droplets (spawned by blasts into water): fly under gravity, expire
  // when they hit the water surface or ground. Purely visual -- the bulk water
  // is a pure function of terrain, so droplets carry no volume.
  if (m_waterSurf && !m_droplets.empty())
  {
    std::vector<Droplet> kept;
    kept.reserve(m_droplets.size());
    for (Droplet& d : m_droplets)
    {
      d.vel.y -= gravity * dt;
      d.pos += d.vel * dt;
      const int x = static_cast<int>(glm::floor(d.pos.x));
      const int z = static_cast<int>(glm::floor(d.pos.z));
      const float ground = m_waterSurf->hasWater(x, z)
                               ? m_waterSurf->surfaceAt(x, z)
                               : static_cast<float>(m_world->surfaceTop(x, z));
      if (d.pos.y > ground)
        kept.push_back(d); // still airborne
    }
    m_droplets.swap(kept);
  }

  for (Debris& d : m_debris)
  {
    d.vel.y -= gravity * dt;
    const glm::ivec3 prev = voxelOf(d.pos);
    const glm::vec3 np = d.pos + d.vel * dt;
    const glm::ivec3 cell = voxelOf(np);

    if (cell != prev && m_world->solidAt(cell.x, cell.y, cell.z))
    {
      // Hit terrain (or piled debris): settle into the last free cell and bake
      // it back into the world as a real voxel.
      bakeDebris(prev, d.color);
      d.life = -1.0f; // mark for removal
      continue;
    }

    d.pos = np;
    d.life -= dt; // a long safety net only (debris persists until it lands)
    if (d.pos.y < -80.0f || d.life < -0.5f)
      d.life = -1.0f;
  }

  m_debris.erase(std::remove_if(m_debris.begin(),
                                m_debris.end(),
                                [](const Debris& d) { return d.life < 0.0f; }),
                 m_debris.end());

  // Rigid blocks: tumble through the air, bounce + roll downhill on the
  // terrain, then bake back in when they come to rest.
  if (!m_blocks.empty())
  {
    ZoneScopedN("Voxel3D::blockPhysics");
    const float g = 170.0f;
    const float fric = glm::clamp(1.0f - 4.0f * dt, 0.0f, 1.0f); // ground drag
    const float spinFric = glm::clamp(1.0f - 3.0f * dt, 0.0f, 1.0f);
    for (Block& b : m_blocks)
    {
      b.vel.y -= g * dt;
      b.pos += b.vel * dt;
      b.angle += b.angVel * dt;

      const int px = static_cast<int>(glm::floor(b.pos.x));
      const int pz = static_cast<int>(glm::floor(b.pos.z));
      const float halfH = static_cast<float>(b.size) * 0.5f;
      const int surf = m_world->surfaceBelow(
          px, static_cast<int>(glm::floor(b.pos.y + halfH)), pz);
      const float restY = static_cast<float>(surf) + halfH;

      if (b.pos.y <= restY)
      {
        b.pos.y = restY;
        b.groundTime += dt;
        // Real impact bounces; a tiny downward speed just rests (no perpetual
        // micro-bounce from gravity vs the ground each frame).
        if (b.vel.y < -20.0f)
          b.vel.y = -b.vel.y * 0.28f;
        else if (b.vel.y < 0.0f)
          b.vel.y = 0.0f;
        b.vel.x *= fric;
        b.vel.z *= fric;
        b.angVel *= spinFric;

        // Slope from neighbour surfaces, sampled WIDE + from HIGH up so the
        // broad hill shape reads (not the fine surface noise) and uphill isn't
        // clipped.
        const int d = b.size + 2;
        const int yTop = surf + 64;
        const int sX = m_world->surfaceBelow(px + d, yTop, pz);
        const int sNX = m_world->surfaceBelow(px - d, yTop, pz);
        const int sZ = m_world->surfaceBelow(px, yTop, pz + d);
        const int sNZ = m_world->surfaceBelow(px, yTop, pz - d);
        glm::vec3 dn{
            static_cast<float>(sNX - sX), 0.0f, static_cast<float>(sNZ - sZ)};
        const float mag = glm::length(dn);
        const float slope = mag / static_cast<float>(2 * d); // rise / run
        if (slope > 0.2f) // steep enough to keep rolling
        {
          dn /= mag;
          b.vel += dn * (slope * 220.0f) * dt;
          b.angVel.x += dn.z * 5.0f * dt;
          b.angVel.z -= dn.x * 5.0f * dt;
        }
        else // flat/gentle: extra static drag so it settles instead of creeping
        {
          b.vel.x *= 0.82f;
          b.vel.z *= 0.82f;
          b.angVel *= 0.82f;
        }

        // Settle when it's been slow for a moment, or simply on the ground too
        // long (a hard fallback so nothing twitches forever).
        const float hsp = glm::length(glm::vec3{b.vel.x, 0.0f, b.vel.z});
        if (hsp < 8.0f)
          b.restTimer += dt;
        else
          b.restTimer = 0.0f;
        if (b.restTimer > 0.25f || b.groundTime > 1.5f)
        {
          bakeBlock(b);
          b.dead = true;
        }
      }
      else
      {
        b.groundTime = 0.0f; // airborne -> reset the contact timers
        b.restTimer = 0.0f;
      }
      if (b.pos.y < -80.0f)
        b.dead = true;
    }
    for (Block& b : m_blocks)
      if (b.dead && b.mesh.vao)
      {
        glDeleteBuffers(1, &b.mesh.vbo);
        glDeleteVertexArrays(1, &b.mesh.vao);
        b.mesh = GpuMesh{};
      }
    m_blocks.erase(std::remove_if(m_blocks.begin(),
                                  m_blocks.end(),
                                  [](const Block& b) { return b.dead; }),
                   m_blocks.end());
  }

  // Flames: emit puffs from the fire's burning voxels, then rise + fade.
  ++m_flameFrame;
  if (m_fire)
  {
    constexpr std::size_t kMaxFlames = 4000;
    for (const glm::ivec3& v : m_fire->burningVoxels())
    {
      if (m_flames.size() >= kMaxFlames)
        break;
      std::uint32_t h = static_cast<std::uint32_t>(v.x) * 73856093u ^
                        static_cast<std::uint32_t>(v.y) * 19349663u ^
                        static_cast<std::uint32_t>(v.z) * 83492791u ^
                        m_flameFrame * 2654435761u;
      h = (h ^ (h >> 13)) * 1274126177u;
      if ((h & 0xFFu) >= 26u)
        continue; // ~10% chance/frame per burning voxel
      const float jx = static_cast<float>((h >> 8) & 0xFu) / 15.0f - 0.5f;
      const float jz = static_cast<float>((h >> 12) & 0xFu) / 15.0f - 0.5f;
      Flame fl;
      fl.pos = glm::vec3{static_cast<float>(v.x) + 0.5f + jx,
                         static_cast<float>(v.y) + 0.8f,
                         static_cast<float>(v.z) + 0.5f + jz};
      fl.vel = glm::vec3{
          jx * 5.0f, 28.0f + static_cast<float>((h >> 16) & 0xFu), jz * 5.0f};
      fl.maxLife = 0.5f + static_cast<float>((h >> 20) & 0xFu) / 30.0f;
      fl.life = fl.maxLife;
      m_flames.push_back(fl);
    }
  }
  if (!m_flames.empty())
  {
    for (Flame& f : m_flames)
    {
      f.vel.y += 12.0f * dt; // buoyancy: accelerate upward
      f.vel.x *= (1.0f - 1.5f * dt);
      f.vel.z *= (1.0f - 1.5f * dt);
      f.pos += f.vel * dt;
      f.life -= dt;
    }
    m_flames.erase(
        std::remove_if(m_flames.begin(),
                       m_flames.end(),
                       [](const Flame& f) { return f.life <= 0.0f; }),
        m_flames.end());
  }
}

void Voxel3DRenderSystem::bakeDebris(const glm::ivec3& cell,
                                     const glm::vec4& color)
{
  // Rise to the first air cell so debris piles instead of overwriting.
  glm::ivec3 c = cell;
  for (int guard = 0; guard < 64 && m_world->solidAt(c.x, c.y, c.z); ++guard)
    ++c.y;

  const auto ch = [](float v)
  {
    return static_cast<std::uint32_t>(
        glm::clamp(static_cast<int>(v * 255.0f), 0, 255));
  };
  const std::uint32_t packed =
      (ch(color.r) << 24) | (ch(color.g) << 16) | (ch(color.b) << 8) | 0xFFu;
  m_world->setVoxel(c.x, c.y, c.z, packed);
}

void Voxel3DRenderSystem::bakeBlock(const Block& b)
{
  const int half = b.size / 2;
  const int bx = static_cast<int>(glm::floor(b.pos.x)) - half;
  const int by = static_cast<int>(glm::floor(b.pos.y)) - half;
  const int bz = static_cast<int>(glm::floor(b.pos.z)) - half;

  for (int lz = 0; lz < b.size; ++lz)
    for (int ly = 0; ly < b.size; ++ly)
      for (int lx = 0; lx < b.size; ++lx)
      {
        const std::uint32_t col = b.voxels[static_cast<std::size_t>(
            (lz * b.size + ly) * b.size + lx)];
        if (col == 0u)
          continue;
        m_world->setVoxel(bx + lx, by + ly, bz + lz, col);
      }
}

void Voxel3DRenderSystem::syncMeshes()
{
  ZoneScopedN("Voxel3D::syncMeshes");
  if (!m_world)
    return;

  // Per-frame caps on MAIN-THREAD work: snapshots built+submitted, and finished
  // meshes uploaded. The meshing itself runs on background workers, so neither
  // a big carve nor a streamed ring stalls the frame -- the work just lands
  // over the next few frames.
  constexpr std::size_t kEnqueueBudget = 64; // higher so flowing water keeps up
  constexpr std::size_t kUploadBudget = 64;

  const auto freeGpu = [this](const glm::ivec3& coord)
  {
    const auto g = m_gpu.find(coord);
    if (g != m_gpu.end())
    {
      for (GpuMesh* m : {&g->second.opaque, &g->second.water})
      {
        if (m->vbo)
          glDeleteBuffers(1, &m->vbo);
        if (m->vao)
          glDeleteVertexArrays(1, &m->vao);
      }
      m_gpu.erase(g);
    }
  };

  // Free GPU meshes for chunks the world unloaded; forget their queue +
  // version.
  for (const glm::ivec3& coord : m_world->unloadedChunks())
  {
    freeGpu(coord);
    m_remeshPending.erase(coord);
    m_chunkGen.erase(coord);
  }
  m_world->clearUnloaded();

  for (const glm::ivec3& coord : m_world->dirtyChunks())
  {
    m_remeshPending.insert(coord);
    if (m_waterSurf) // terrain (re)meshed -> its water tile must rebuild too
      m_waterSurf->touchTile(coord.x, coord.z);
  }
  m_world->clearDirty();

  // Submit a budget of mesh jobs: snapshot each chunk (cheap, main thread) and
  // mesh it on a worker. Bump the chunk's generation so a stale result (the
  // chunk was edited again) is discarded on arrival.
  if (!m_remeshPending.empty())
  {
    ZoneScopedN("Voxel3D::enqueueMesh");
    const TinyVoxelWorld::ChunkMap& chunks = m_world->chunks();
    std::vector<glm::ivec3> taken;
    taken.reserve(kEnqueueBudget);
    for (const glm::ivec3& coord : m_remeshPending)
    {
      if (taken.size() >= kEnqueueBudget)
        break;
      taken.push_back(coord);
      if (chunks.find(coord) == chunks.end())
      {
        freeGpu(coord); // no data (all-air or unloaded): drop any stale mesh
        m_chunkGen.erase(coord);
        continue;
      }
      const std::uint64_t gen = ++m_chunkGen[coord];
      MeshSnapshot snap = buildSnapshot(*m_world, coord, gen);
      m_meshQueue.submit(
          [this, snap = std::move(snap)]
          {
            ZoneScopedN("Voxel3D::meshAsync");
            TinyChunkMesh mesh = meshSnapshot(snap);
            std::lock_guard<std::mutex> lock(m_meshResultsMutex);
            m_meshResults.push_back({snap.coord, snap.gen, std::move(mesh)});
          });
    }
    for (const glm::ivec3& coord : taken)
      m_remeshPending.erase(coord);
  }

  // Drain finished meshes and upload a budget (skip any whose generation is
  // stale
  // -- a newer edit re-queued it, or it was unloaded).
  {
    ZoneScopedN("Voxel3D::uploadChunks");
    std::vector<MeshResult> ready;
    {
      std::lock_guard<std::mutex> lock(m_meshResultsMutex);
      const std::size_t take = std::min(kUploadBudget, m_meshResults.size());
      ready.insert(ready.end(),
                   std::make_move_iterator(m_meshResults.end() - take),
                   std::make_move_iterator(m_meshResults.end()));
      m_meshResults.resize(m_meshResults.size() - take);
    }
    for (MeshResult& r : ready)
    {
      const auto g = m_chunkGen.find(r.coord);
      if (g != m_chunkGen.end() && g->second == r.gen)
      {
        ChunkGpu& cg = m_gpu[r.coord];
        uploadMesh(cg.opaque, r.mesh.opaque);
        uploadMesh(cg.water, r.mesh.water);
      }
    }
  }
}

void Voxel3DRenderSystem::render()
{
  ZoneScopedN("Voxel3D::render");
  ensureInitialized();
  if (m_program == 0 || !m_world)
    return;

  syncMeshes();

  m_camera.aspect =
      static_cast<float>(m_viewportW) / static_cast<float>(m_viewportH);

  glViewport(0, 0, m_viewportW, m_viewportH);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);

  glUseProgram(m_program);
  const glm::mat4 viewProj = m_camera.viewProj();
  glUniformMatrix4fv(m_uViewProj, 1, GL_FALSE, glm::value_ptr(viewProj));
  const glm::mat4 identity{1.0f};
  glUniformMatrix4fv(m_uModel, 1, GL_FALSE, glm::value_ptr(identity));
  glUniform1f(m_uEmissive, 0.0f); // lit by default; flames flip it on
  const glm::vec3 l = m_lightDir;
  glUniform3f(m_uLightDir, l.x, l.y, l.z);
  glUniform3f(m_uSunColor, m_sunColor.x, m_sunColor.y, m_sunColor.z);
  glUniform1f(m_uAmbient, m_ambient);
  glUniform1f(m_uSunDiffuse, m_sunDiffuse);

  // Point lights, constant for every draw this frame.
  int nLights = static_cast<int>(m_lights.size());
  if (nLights > 16)
    nLights = 16;
  glUniform1i(m_uLightCount, nLights);
  if (nLights > 0)
  {
    float pos[16 * 3];
    float col[16 * 3];
    float rad[16];
    float inten[16];
    for (int i = 0; i < nLights; ++i)
    {
      pos[i * 3 + 0] = m_lights[i].pos.x;
      pos[i * 3 + 1] = m_lights[i].pos.y;
      pos[i * 3 + 2] = m_lights[i].pos.z;
      col[i * 3 + 0] = m_lights[i].color.x;
      col[i * 3 + 1] = m_lights[i].color.y;
      col[i * 3 + 2] = m_lights[i].color.z;
      rad[i] = m_lights[i].radius;
      inten[i] = m_lights[i].intensity;
    }
    glUniform3fv(m_uLightPos, nLights, pos);
    glUniform3fv(m_uLightColor, nLights, col);
    glUniform1fv(m_uLightRadius, nLights, rad);
    glUniform1fv(m_uLightIntensity, nLights, inten);
  }

  // Opaque pass: terrain + player (their vertex alpha is 1).
  {
    ZoneScopedN("Voxel3D::drawTerrain");
    for (const auto& [coord, cg] : m_gpu)
    {
      if (cg.opaque.vertexCount == 0)
        continue;
      glBindVertexArray(cg.opaque.vao);
      glDrawArrays(GL_TRIANGLES, 0, cg.opaque.vertexCount);
    }
  }

  if (m_hasPlayer)
  {
    uploadMesh(
        m_playerMesh, buildBox(m_playerCenter, m_playerHalf, m_playerColor));
    glBindVertexArray(m_playerMesh.vao);
    glDrawArrays(GL_TRIANGLES, 0, m_playerMesh.vertexCount);
  }

  // Debris pass: flying single-voxel chunks, rebuilt each frame. Opaque
  // (they're real voxels) and depth-tested like terrain.
  if (!m_debris.empty())
  {
    ZoneScopedN("Voxel3D::debrisPass"); // build + upload + draw
    uploadMesh(m_debrisMesh, buildDebrisMesh(m_debris, 0.5f));
    glBindVertexArray(m_debrisMesh.vao);
    glDrawArrays(GL_TRIANGLES, 0, m_debrisMesh.vertexCount);
  }

  // Rigid blocks: tumbling voxel chunks. Each block's local mesh is built once;
  // the tumble + position are a per-block model matrix on the GPU (no per-frame
  // CPU rotation or re-upload).
  if (!m_blocks.empty())
  {
    ZoneScopedN("Voxel3D::blockPass");
    for (Block& b : m_blocks)
    {
      if (b.mesh.vao == 0)
        uploadMesh(b.mesh, buildBlockLocalMesh(b));
      glm::mat4 model = glm::translate(glm::mat4(1.0f), b.pos);
      model = glm::rotate(model, b.angle.x, {1, 0, 0});
      model = glm::rotate(model, b.angle.y, {0, 1, 0});
      model = glm::rotate(model, b.angle.z, {0, 0, 1});
      glUniformMatrix4fv(m_uModel, 1, GL_FALSE, glm::value_ptr(model));
      glBindVertexArray(b.mesh.vao);
      glDrawArrays(GL_TRIANGLES, 0, b.mesh.vertexCount);
    }
    glUniformMatrix4fv(m_uModel, 1, GL_FALSE, glm::value_ptr(identity));
  }

  // Translucent water pass: the heightfield surface meshed in 32-col TILES +
  // the flying droplets, blended with no depth write so the bed shows through.
  // Only new or dirtied in-view tiles re-mesh, a budget per frame, so a big
  // re-level amortizes instead of re-uploading the whole lake every frame.
  if (m_waterSurf)
  {
    ZoneScopedN("Voxel3D::waterPass");
    constexpr int kTileBuildBudget = 64; // keep flowing tiles up to date
    const int cx = static_cast<int>(glm::floor(m_camera.focus.x));
    const int cz = static_cast<int>(glm::floor(m_camera.focus.z));
    const int vr = static_cast<int>(m_camera.zoom * 1.7f) + 48;
    const glm::ivec2 lo = WaterSurfaceSystem::tileOf(cx - vr, cz - vr);
    const glm::ivec2 hi = WaterSurfaceSystem::tileOf(cx + vr, cz + vr);

    for (auto it = m_waterTiles.begin(); it != m_waterTiles.end();)
    {
      if (it->first.x < lo.x || it->first.x > hi.x || it->first.y < lo.y ||
          it->first.y > hi.y)
      {
        if (it->second.vbo)
          glDeleteBuffers(1, &it->second.vbo);
        if (it->second.vao)
          glDeleteVertexArrays(1, &it->second.vao);
        it = m_waterTiles.erase(it);
      }
      else
        ++it;
    }

    int budget = kTileBuildBudget;
    for (int tz = lo.y; tz <= hi.y && budget > 0; ++tz)
      for (int tx = lo.x; tx <= hi.x && budget > 0; ++tx)
      {
        const glm::ivec2 t{tx, tz};
        if (m_waterTiles.count(t) && !m_waterSurf->tileDirty(tx, tz))
          continue;
        uploadMesh(m_waterTiles[t],
                   buildWaterTileMesh(*m_waterSurf, *m_world, tx, tz));
        m_waterSurf->clearTile(tx, tz);
        --budget;
      }

    if (!m_droplets.empty())
      uploadMesh(m_dropletMesh, buildDropletMesh(m_droplets));

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    for (const auto& [t, mesh] : m_waterTiles)
    {
      if (mesh.vertexCount == 0)
        continue;
      glBindVertexArray(mesh.vao);
      glDrawArrays(GL_TRIANGLES, 0, mesh.vertexCount);
    }
    if (!m_droplets.empty() && m_dropletMesh.vertexCount > 0)
    {
      glBindVertexArray(m_dropletMesh.vao);
      glDrawArrays(GL_TRIANGLES, 0, m_dropletMesh.vertexCount);
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
  }

  // Flame + ember pass: emissive, additive cubes drawn last so they glow over
  // the scene. Depth-tested (terrain occludes them) but no depth write.
  const bool hasEmbers = m_fire && !m_fire->embers().empty();
  if (!m_flames.empty() || hasEmbers)
  {
    ZoneScopedN("Voxel3D::flamePass");
    glUniform1f(m_uEmissive, 1.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE); // additive
    glDepthMask(GL_FALSE);
    if (!m_flames.empty())
    {
      uploadMesh(m_flameMesh, buildFlameMesh(m_flames));
      glBindVertexArray(m_flameMesh.vao);
      glDrawArrays(GL_TRIANGLES, 0, m_flameMesh.vertexCount);
    }
    if (hasEmbers)
    {
      uploadMesh(m_emberMesh, buildEmberMesh(m_fire->embers()));
      glBindVertexArray(m_emberMesh.vao);
      glDrawArrays(GL_TRIANGLES, 0, m_emberMesh.vertexCount);
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glUniform1f(m_uEmissive, 0.0f);
  }

  glBindVertexArray(0);
  glUseProgram(0);
}

} // namespace sfs
