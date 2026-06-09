#include "engine/runtime/systems/voxel3DRenderSystem.h"

#include "engine/core/logger/logger.h"
#include "engine/core/voxel/tinyVoxelMesher.h"
#include "engine/generated/embeddedShaders.h"
#include "glm/glm/gtc/type_ptr.hpp"

#ifdef __EMSCRIPTEN__
  #include <GLES3/gl3.h>
#else
  #include <GL/glew.h>
#endif

#include <cstddef>
#include <string>
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
      3,
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
  const auto quad = [&](int a, int b, int d, int e, const glm::vec3& n)
  {
    out.push_back({c[a], n, color});
    out.push_back({c[b], n, color});
    out.push_back({c[d], n, color});
    out.push_back({c[a], n, color});
    out.push_back({c[d], n, color});
    out.push_back({c[e], n, color});
  };
  quad(1, 2, 6, 5, {1, 0, 0});  // +x
  quad(0, 4, 7, 3, {-1, 0, 0}); // -x
  quad(3, 7, 6, 2, {0, 1, 0});  // +y
  quad(0, 1, 5, 4, {0, -1, 0}); // -y
  quad(4, 5, 6, 7, {0, 0, 1});  // +z
  quad(0, 3, 2, 1, {0, 0, -1}); // -z
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

  glBindVertexArray(0);
  glUseProgram(0);
}

} // namespace sfs
