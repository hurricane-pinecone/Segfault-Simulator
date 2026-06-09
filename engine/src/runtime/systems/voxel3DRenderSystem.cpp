#include "engine/runtime/systems/voxel3DRenderSystem.h"

#include "engine/core/logger/logger.h"
#include "engine/core/voxel/tinyVoxelMesher.h"
#include "engine/generated/embeddedShaders.h"
#include "glm/glm/common.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/gtc/type_ptr.hpp"
#include "glm/glm/trigonometric.hpp"

#ifdef __EMSCRIPTEN__
  #include <GLES3/gl3.h>
#else
  #include <GL/glew.h>
#endif

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sfs
{
namespace
{

int floorDiv(int a, int b)
{
  int q = a / b;
  int r = a % b;
  if (r != 0 && ((r < 0) != (b < 0)))
    --q;
  return q;
}

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

// Build the animated water surface as a VOXEL heightfield, rebuilt every frame
// (the "moving voxels" workload that gauges performance). Each column's wave
// height is QUANTIZED to an integer voxel level, so the surface is flat-topped
// per cell with vertical STEP faces where a cell rises above a lower water
// neighbour -- it reads as little cubes, not a smooth sheet. Depth (sea -
// floor) tints shallow->deep, plus per-voxel brightness; drawn transparent.
std::vector<TinyVoxelVertex>
buildWaterMesh(const std::vector<Voxel3DRenderSystem::WaterColumn>& water,
               int sea,
               double time)
{
  const float t = static_cast<float>(time);
  const float amp = 4.0f;
  const auto key = [](int x, int z)
  {
    return (static_cast<std::int64_t>(static_cast<std::uint32_t>(x)) << 32) |
           static_cast<std::uint32_t>(z);
  };

  // Pass 1: quantized wave top per water column.
  std::unordered_map<std::int64_t, int> top;
  top.reserve(water.size() * 2);
  for (const auto& w : water)
  {
    const float fx = static_cast<float>(w.x);
    const float fz = static_cast<float>(w.z);
    const float wave = amp * (0.5f * (glm::sin(0.18f * fx + 1.3f * t) +
                                      glm::sin(0.15f * fz + 1.0f * t)) +
                              0.4f * glm::sin(0.07f * (fx + fz) + 0.6f * t));
    top[key(w.x, w.z)] =
        static_cast<int>(glm::floor(static_cast<float>(sea) + wave + 0.5f));
  }

  const glm::vec3 shallow{0.42f, 0.68f, 0.84f};
  const glm::vec3 deep{0.10f, 0.26f, 0.56f};
  const auto vary = [](int x, int y, int z)
  {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093u ^
                      static_cast<std::uint32_t>(y) * 19349663u ^
                      static_cast<std::uint32_t>(z) * 83492791u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return 0.9f + 0.2f * (static_cast<float>(h & 0xFFFFu) / 65535.0f);
  };

  std::vector<TinyVoxelVertex> out;
  out.reserve(water.size() * 9);
  const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
  for (const auto& w : water)
  {
    const int ty = top[key(w.x, w.z)];
    const float x = static_cast<float>(w.x);
    const float z = static_cast<float>(w.z);
    const float y = static_cast<float>(ty);
    const float depth =
        glm::clamp(static_cast<float>(sea - w.floorY) / 80.0f, 0.0f, 1.0f);
    // Deeper = darker (colour) AND more opaque (alpha), so the bottom shows in
    // the shallows and is hidden in deep water, like the sample game.
    const float alpha = glm::mix(0.62f, 0.96f, depth);
    const glm::vec4 color{
        glm::mix(shallow, deep, depth) * vary(w.x, ty, w.z), alpha};

    // Flat top face (voxel top), normal up.
    const glm::vec3 up{0.0f, 1.0f, 0.0f};
    out.push_back({{x, y, z}, up, color});
    out.push_back({{x + 1, y, z}, up, color});
    out.push_back({{x + 1, y, z + 1}, up, color});
    out.push_back({{x, y, z}, up, color});
    out.push_back({{x + 1, y, z + 1}, up, color});
    out.push_back({{x, y, z + 1}, up, color});

    // Vertical step faces down to any LOWER water neighbour (the voxel risers).
    for (const auto& d : dirs)
    {
      const auto it = top.find(key(w.x + d[0], w.z + d[1]));
      if (it == top.end() || it->second >= ty)
        continue; // higher water (hidden) or land shore (occluded by terrain)
      const float lo = static_cast<float>(it->second);
      const glm::vec3 nrm{
          static_cast<float>(d[0]), 0.0f, static_cast<float>(d[1])};
      // The shared edge between this cell and the neighbour, lo..ty tall.
      float ex0, ez0, ex1, ez1;
      if (d[0] == 1)
      {
        ex0 = x + 1;
        ez0 = z;
        ex1 = x + 1;
        ez1 = z + 1;
      }
      else if (d[0] == -1)
      {
        ex0 = x;
        ez0 = z + 1;
        ex1 = x;
        ez1 = z;
      }
      else if (d[1] == 1)
      {
        ex0 = x + 1;
        ez0 = z + 1;
        ex1 = x;
        ez1 = z + 1;
      }
      else
      {
        ex0 = x;
        ez0 = z;
        ex1 = x + 1;
        ez1 = z;
      }
      out.push_back({{ex0, lo, ez0}, nrm, color});
      out.push_back({{ex1, lo, ez1}, nrm, color});
      out.push_back({{ex1, y, ez1}, nrm, color});
      out.push_back({{ex0, lo, ez0}, nrm, color});
      out.push_back({{ex1, y, ez1}, nrm, color});
      out.push_back({{ex0, y, ez0}, nrm, color});
    }
  }
  return out;
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
      m_viewportH(windowHeight > 0 ? windowHeight : 1)
{
}

Voxel3DRenderSystem::~Voxel3DRenderSystem()
{
  for (auto& [coord, mesh] : m_gpu)
  {
    if (mesh.vbo)
      glDeleteBuffers(1, &mesh.vbo);
    if (mesh.vao)
      glDeleteVertexArrays(1, &mesh.vao);
  }
  if (m_playerMesh.vbo)
    glDeleteBuffers(1, &m_playerMesh.vbo);
  if (m_playerMesh.vao)
    glDeleteVertexArrays(1, &m_playerMesh.vao);
  if (m_program)
    glDeleteProgram(m_program);
}

void Voxel3DRenderSystem::setViewport(int width, int height)
{
  m_viewportW = width > 0 ? width : 1;
  m_viewportH = height > 0 ? height : 1;
}

void Voxel3DRenderSystem::setChunk(const glm::ivec3& chunkCoord,
                                   const TinyVoxelChunk& chunk)
{
  m_chunks[chunkCoord] = chunk;
  m_dirty.insert(chunkCoord);
  // A border voxel can expose/hide a neighbour's face, so re-mesh neighbours
  // too.
  m_dirty.insert(chunkCoord + glm::ivec3{1, 0, 0});
  m_dirty.insert(chunkCoord + glm::ivec3{-1, 0, 0});
  m_dirty.insert(chunkCoord + glm::ivec3{0, 1, 0});
  m_dirty.insert(chunkCoord + glm::ivec3{0, -1, 0});
  m_dirty.insert(chunkCoord + glm::ivec3{0, 0, 1});
  m_dirty.insert(chunkCoord + glm::ivec3{0, 0, -1});
}

bool Voxel3DRenderSystem::solidWorld(int wx, int wy, int wz) const
{
  const glm::ivec3 c{floorDiv(wx, kTinyChunkSize),
                     floorDiv(wy, kTinyChunkSize),
                     floorDiv(wz, kTinyChunkSize)};
  const auto it = m_chunks.find(c);
  if (it == m_chunks.end())
    return false;
  return it->second.solid(wx - c.x * kTinyChunkSize,
                          wy - c.y * kTinyChunkSize,
                          wz - c.z * kTinyChunkSize);
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
  m_uLightDir = glGetUniformLocation(m_program, "uLightDir");
}

void Voxel3DRenderSystem::update(double deltaTime) { m_time += deltaTime; }

void Voxel3DRenderSystem::remeshDirty()
{
  if (m_dirty.empty())
    return;

  const auto solid = [this](int x, int y, int z)
  { return solidWorld(x, y, z); };

  for (const glm::ivec3& coord : m_dirty)
  {
    const auto it = m_chunks.find(coord);
    if (it == m_chunks.end())
      continue; // neighbour slot with no data yet

    const std::vector<TinyVoxelVertex> verts =
        meshTinyChunk(coord * kTinyChunkSize, it->second, solid);
    uploadMesh(m_gpu[coord], verts);
  }
  m_dirty.clear();
}

void Voxel3DRenderSystem::render()
{
  ensureInitialized();
  if (m_program == 0)
    return;

  remeshDirty();

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
  const glm::vec3 l = m_lightDir;
  glUniform3f(m_uLightDir, l.x, l.y, l.z);

  // Opaque pass: terrain + player (their vertex alpha is 1).
  for (const auto& [coord, mesh] : m_gpu)
  {
    if (mesh.vertexCount == 0)
      continue;
    glBindVertexArray(mesh.vao);
    glDrawArrays(GL_TRIANGLES, 0, mesh.vertexCount);
  }

  if (m_hasPlayer)
  {
    uploadMesh(
        m_playerMesh, buildBox(m_playerCenter, m_playerHalf, m_playerColor));
    glBindVertexArray(m_playerMesh.vao);
    glDrawArrays(GL_TRIANGLES, 0, m_playerMesh.vertexCount);
  }

  // Transparent water pass: rebuild the animated wave surface, blend over the
  // opaque scene, and don't write depth (so the bed stays visible through it).
  if (!m_water.empty())
  {
    uploadMesh(m_waterMesh, buildWaterMesh(m_water, m_seaLevel, m_time));
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glBindVertexArray(m_waterMesh.vao);
    glDrawArrays(GL_TRIANGLES, 0, m_waterMesh.vertexCount);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
  }

  glBindVertexArray(0);
  glUseProgram(0);
}

} // namespace sfs
