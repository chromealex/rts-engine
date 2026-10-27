// OpenGL entry points used by the GL backend.
//  * Android / Linux (EGL) / Web (Emscripten): link GLES3 directly.
//  * Windows: desktop GL 3.3 core, functions above GL 1.1 are loaded at runtime via
//    wglGetProcAddress (no GLEW/GLAD dependency).
#pragma once

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <windows.h>
  #include <GL/gl.h>
  #include <cstddef>
  #define RTS_GL_LOADER 1
  typedef char GLchar;
  typedef ptrdiff_t GLsizeiptr;
  typedef ptrdiff_t GLintptr;
  #define GL_ARRAY_BUFFER 0x8892
  #define GL_ELEMENT_ARRAY_BUFFER 0x8893
  #define GL_UNIFORM_BUFFER 0x8A11
  #define GL_STATIC_DRAW 0x88E4
  #define GL_DYNAMIC_DRAW 0x88E8
  #define GL_STREAM_DRAW 0x88E0
  #define GL_FRAGMENT_SHADER 0x8B30
  #define GL_VERTEX_SHADER 0x8B31
  #define GL_COMPILE_STATUS 0x8B81
  #define GL_LINK_STATUS 0x8B82
  #define GL_INFO_LOG_LENGTH 0x8B84
  #define GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT 0x8A34
  #define GL_INVALID_INDEX 0xFFFFFFFFu
  #define GL_TEXTURE0 0x84C0
  #define GL_CLAMP_TO_EDGE 0x812F
  #define GL_R8 0x8229
  #define GL_RED 0x1903
  #define GL_RGBA8 0x8058
  #define GL_FUNC_ADD 0x8006
  #define GL_FRAMEBUFFER_SRGB 0x8DB9
  #define GL_DEPTH_COMPONENT24 0x81A6
  #define GL_TEXTURE_COMPARE_MODE 0x884C
  #define GL_TEXTURE_COMPARE_FUNC 0x884D
  #define GL_COMPARE_REF_TO_TEXTURE 0x884E
  #define GL_FRAMEBUFFER 0x8D40
  #define GL_COLOR_ATTACHMENT0 0x8CE0
  #define GL_DEPTH_ATTACHMENT 0x8D00
  #define GL_FRAMEBUFFER_COMPLETE 0x8CD5
  #define RTS_GL_FUNCS(X) \
    X(void, glGenBuffers, (GLsizei n, GLuint* b)) \
    X(void, glDeleteBuffers, (GLsizei n, const GLuint* b)) \
    X(void, glBindBuffer, (GLenum t, GLuint b)) \
    X(void, glBufferData, (GLenum t, GLsizeiptr s, const void* d, GLenum u)) \
    X(void, glBufferSubData, (GLenum t, GLintptr o, GLsizeiptr s, const void* d)) \
    X(void, glBindBufferRange, (GLenum t, GLuint i, GLuint b, GLintptr o, GLsizeiptr s)) \
    X(void, glGenVertexArrays, (GLsizei n, GLuint* a)) \
    X(void, glDeleteVertexArrays, (GLsizei n, const GLuint* a)) \
    X(void, glBindVertexArray, (GLuint a)) \
    X(void, glEnableVertexAttribArray, (GLuint i)) \
    X(void, glDisableVertexAttribArray, (GLuint i)) \
    X(void, glVertexAttribPointer, (GLuint i, GLint s, GLenum t, GLboolean n, GLsizei st, const void* p)) \
    X(GLuint, glCreateShader, (GLenum t)) \
    X(void, glDeleteShader, (GLuint s)) \
    X(void, glShaderSource, (GLuint s, GLsizei c, const GLchar* const* str, const GLint* len)) \
    X(void, glCompileShader, (GLuint s)) \
    X(void, glGetShaderiv, (GLuint s, GLenum p, GLint* v)) \
    X(void, glGetShaderInfoLog, (GLuint s, GLsizei m, GLsizei* l, GLchar* log)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glDeleteProgram, (GLuint p)) \
    X(void, glAttachShader, (GLuint p, GLuint s)) \
    X(void, glBindAttribLocation, (GLuint p, GLuint i, const GLchar* n)) \
    X(void, glLinkProgram, (GLuint p)) \
    X(void, glGetProgramiv, (GLuint p, GLenum e, GLint* v)) \
    X(void, glGetProgramInfoLog, (GLuint p, GLsizei m, GLsizei* l, GLchar* log)) \
    X(void, glUseProgram, (GLuint p)) \
    X(GLint, glGetUniformLocation, (GLuint p, const GLchar* n)) \
    X(void, glUniform1i, (GLint l, GLint v)) \
    X(GLuint, glGetUniformBlockIndex, (GLuint p, const GLchar* n)) \
    X(void, glUniformBlockBinding, (GLuint p, GLuint i, GLuint b)) \
    X(void, glActiveTexture, (GLenum t)) \
    X(void, glBlendFuncSeparate, (GLenum a, GLenum b, GLenum c, GLenum d)) \
    X(void, glBlendEquation, (GLenum m)) \
    X(void, glGenFramebuffers, (GLsizei n, GLuint* f)) \
    X(void, glDeleteFramebuffers, (GLsizei n, const GLuint* f)) \
    X(void, glBindFramebuffer, (GLenum t, GLuint f)) \
    X(void, glFramebufferTexture2D, (GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum t)) \
    X(void, glDrawBuffers, (GLsizei n, const GLenum* b))
  #define RTS_GL_DECLARE(ret, name, args) typedef ret (APIENTRY* PFN_##name) args; extern PFN_##name name;
  RTS_GL_FUNCS(RTS_GL_DECLARE)
  #undef RTS_GL_DECLARE
  namespace rts::gfx { bool loadGLFunctions(); }
#elif defined(__EMSCRIPTEN__)
  #include <GLES3/gl3.h>
#else
  #include <GLES3/gl3.h>
#endif
