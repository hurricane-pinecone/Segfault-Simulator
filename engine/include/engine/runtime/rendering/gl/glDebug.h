#pragma once

#ifdef __EMSCRIPTEN__
  #include <GLES3/gl3.h>
#else
  #include <GL/glew.h>
#endif

#include "engine/core/logger/logger.h"
#include <string>

namespace sfs::gl
{

/** Human-readable name for a GL error enum (numeric fallback for unknowns). */
inline std::string errorName(GLenum err)
{
  switch (err)
  {
  case GL_INVALID_ENUM:
    return "GL_INVALID_ENUM";
  case GL_INVALID_VALUE:
    return "GL_INVALID_VALUE";
  case GL_INVALID_OPERATION:
    return "GL_INVALID_OPERATION";
  case GL_INVALID_FRAMEBUFFER_OPERATION:
    return "GL_INVALID_FRAMEBUFFER_OPERATION";
  case GL_OUT_OF_MEMORY:
    return "GL_OUT_OF_MEMORY";
  default:
    return "GL_ERROR(0x" + std::to_string(err) + ")";
  }
}

/**
 * Drain the GL error queue, logging each pending error against a call-site tag.
 * The driver accumulates errors until queried, so this reports every error
 * raised since the previous drain.
 *
 * @return true if any error was reported.
 */
inline bool
checkErrors(const char* tag, const char* file, int line, const char* func)
{
  bool any = false;
  for (GLenum err = glGetError(); err != GL_NO_ERROR; err = glGetError())
  {
    any = true;
    Logger::error(
        "GL error " + errorName(err) + " at " + tag, file, line, func);
  }
  return any;
}

/**
 * glGetUniformLocation that flags a missing uniform. A location of -1 means the
 * name isn't an active uniform (a typo, or a uniform the compiler dropped as
 * unused) and the matching glUniform* call silently no-ops. Logged at debug
 * level since an optimised-out uniform is a benign cause of -1.
 */
inline GLint uniformLocation(GLuint program,
                             const char* name,
                             const char* file,
                             int line,
                             const char* func)
{
  GLint loc = glGetUniformLocation(program, name);
  if (loc < 0)
    Logger::debug(std::string("uniform '") + name + "' not found (location -1)",
                  file,
                  line,
                  func);
  return loc;
}

} // namespace sfs::gl

/** Drain + log any pending GL errors, tagging them with the call site. */
#define SFS_GL_CHECK(tag)                                                      \
  sfs::gl::checkErrors(                                                        \
      (tag), sfs::__log_file_name(__FILE__), __LINE__, SFS_PRETTY_FUNCTION)

/** Checked glGetUniformLocation: warns (debug) when the uniform is absent. */
#define SFS_GL_UNIFORM(program, name)                                          \
  sfs::gl::uniformLocation((program),                                          \
                           (name),                                             \
                           sfs::__log_file_name(__FILE__),                     \
                           __LINE__,                                           \
                           SFS_PRETTY_FUNCTION)
