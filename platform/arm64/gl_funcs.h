// Minimal dynamic OpenGL ES 2.0 / Desktop OpenGL 2.1 function loader via SDL2.
// Nothing links against libGLESv2.so or libGL.so directly, so the binary runs
// unchanged across Mali-G31 (libmali), Panfrost, Lima, Mesa, and desktop X11/Wayland.
#pragma once

#include <SDL.h>
#include <cstddef>
#include <cstdint>

#ifndef APIENTRY
#define APIENTRY
#endif

using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLint = int;
using GLuint = unsigned int;
using GLsizei = int;
using GLfloat = float;
using GLdouble = double;
using GLchar = char;
using GLubyte = unsigned char;
using GLintptr = std::ptrdiff_t;
using GLsizeiptr = std::ptrdiff_t;

inline constexpr GLboolean GL_FALSE = 0;
inline constexpr GLboolean GL_TRUE = 1;
inline constexpr GLbitfield GL_DEPTH_BUFFER_BIT = 0x00000100u;
inline constexpr GLbitfield GL_STENCIL_BUFFER_BIT = 0x00000400u;
inline constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x00004000u;
inline constexpr GLenum GL_TRIANGLES = 0x0004u;
inline constexpr GLenum GL_EQUAL = 0x0202u;
inline constexpr GLenum GL_GREATER = 0x0204u;
inline constexpr GLenum GL_GEQUAL = 0x0206u;
inline constexpr GLenum GL_ALWAYS = 0x0207u;
inline constexpr GLenum GL_SRC_ALPHA = 0x0302u;
inline constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303u;
inline constexpr GLenum GL_CULL_FACE = 0x0B44u;
inline constexpr GLenum GL_DEPTH_TEST = 0x0B71u;
inline constexpr GLenum GL_STENCIL_TEST = 0x0B90u;
inline constexpr GLenum GL_BLEND = 0x0BE2u;
inline constexpr GLenum GL_SCISSOR_TEST = 0x0C11u;
inline constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5u;
inline constexpr GLenum GL_MAX_TEXTURE_SIZE = 0x0D33u;
inline constexpr GLenum GL_STENCIL_BITS = 0x0D57u;
inline constexpr GLenum GL_TEXTURE_2D = 0x0DE1u;
inline constexpr GLenum GL_UNSIGNED_BYTE = 0x1401u;
inline constexpr GLenum GL_UNSIGNED_SHORT = 0x1403u;
inline constexpr GLenum GL_FLOAT = 0x1406u;
inline constexpr GLenum GL_INVERT = 0x150Au;
inline constexpr GLenum GL_RGB = 0x1907u;
inline constexpr GLenum GL_RGBA = 0x1908u;
inline constexpr GLenum GL_VENDOR = 0x1F00u;
inline constexpr GLenum GL_RENDERER = 0x1F01u;
inline constexpr GLenum GL_VERSION = 0x1F02u;
inline constexpr GLenum GL_KEEP = 0x1E00u;
inline constexpr GLenum GL_REPLACE = 0x1E01u;
inline constexpr GLenum GL_NEAREST = 0x2600u;
inline constexpr GLenum GL_LINEAR = 0x2601u;
inline constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800u;
inline constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801u;
inline constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802u;
inline constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803u;
inline constexpr GLenum GL_UNSIGNED_SHORT_5_5_5_1 = 0x8034u;
inline constexpr GLenum GL_CLAMP_TO_EDGE = 0x812Fu;
inline constexpr GLenum GL_UNSIGNED_SHORT_5_6_5 = 0x8363u;
inline constexpr GLenum GL_TEXTURE0 = 0x84C0u;
inline constexpr GLenum GL_ARRAY_BUFFER = 0x8892u;
inline constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893u;
inline constexpr GLenum GL_STREAM_DRAW = 0x88E0u;
inline constexpr GLenum GL_STATIC_DRAW = 0x88E4u;
inline constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30u;
inline constexpr GLenum GL_VERTEX_SHADER = 0x8B31u;
inline constexpr GLenum GL_COMPILE_STATUS = 0x8B81u;
inline constexpr GLenum GL_LINK_STATUS = 0x8B82u;

#define ARM64_GL_FUNCS(X) \
    X(void, glActiveTexture, (GLenum)) \
    X(void, glAttachShader, (GLuint, GLuint)) \
    X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void, glBindBuffer, (GLenum, GLuint)) \
    X(void, glBindTexture, (GLenum, GLuint)) \
    X(void, glBlendFunc, (GLenum, GLenum)) \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void, glClear, (GLbitfield)) \
    X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glClearStencil, (GLint)) \
    X(void, glCompileShader, (GLuint)) \
    X(GLuint, glCreateProgram, (void)) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void, glDeleteBuffers, (GLsizei, const GLuint *)) \
    X(void, glDeleteProgram, (GLuint)) \
    X(void, glDeleteShader, (GLuint)) \
    X(void, glDeleteTextures, (GLsizei, const GLuint *)) \
    X(void, glDepthMask, (GLboolean)) \
    X(void, glDisable, (GLenum)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
    X(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *)) \
    X(void, glEnable, (GLenum)) \
    X(void, glEnableVertexAttribArray, (GLuint)) \
    X(void, glFinish, (void)) \
    X(void, glGenBuffers, (GLsizei, GLuint *)) \
    X(void, glGenTextures, (GLsizei, GLuint *)) \
    X(GLenum, glGetError, (void)) \
    X(void, glGetIntegerv, (GLenum, GLint *)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
    X(const GLubyte *, glGetString, (GLenum)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar *)) \
    X(void, glLinkProgram, (GLuint)) \
    X(void, glPixelStorei, (GLenum, GLint)) \
    X(void, glScissor, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, glStencilFunc, (GLenum, GLint, GLuint)) \
    X(void, glStencilMask, (GLuint)) \
    X(void, glStencilOp, (GLenum, GLenum, GLenum)) \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, glTexParameteri, (GLenum, GLenum, GLint)) \
    X(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, glUniform1f, (GLint, GLfloat)) \
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glUniform2f, (GLint, GLfloat, GLfloat)) \
    X(void, glUseProgram, (GLuint)) \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei))

namespace arm64 {

struct GlFuncs {
#define ARM64_GL_FIELD(ret, name, args) ret (APIENTRY *name) args = nullptr;
    ARM64_GL_FUNCS(ARM64_GL_FIELD)
#undef ARM64_GL_FIELD

    bool load() {
        bool ok = true;
#define ARM64_GL_LOAD(ret, name, args) \
        name = reinterpret_cast<ret (APIENTRY *) args>(SDL_GL_GetProcAddress(#name)); \
        if (!name) ok = false;
        ARM64_GL_FUNCS(ARM64_GL_LOAD)
#undef ARM64_GL_LOAD
        return ok;
    }
};

} // namespace arm64
