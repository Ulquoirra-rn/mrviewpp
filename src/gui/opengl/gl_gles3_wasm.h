/* Auto-generated GLES3/WebGL2 alias for the mrtrix gl:: namespace (WASM build). */
#ifndef __gui_opengl_gl_gles3_wasm_h__
#define __gui_opengl_gl_gles3_wasm_h__
#include <GLES3/gl3.h>
namespace MR { namespace GUI { namespace GL_ {} } }
namespace gl {
  // ---- enums ----
  constexpr GLenum ALIASED_LINE_WIDTH_RANGE = GL_ALIASED_LINE_WIDTH_RANGE;
  constexpr GLenum ARRAY_BUFFER = GL_ARRAY_BUFFER;
  constexpr GLenum BACK = GL_BACK;
  constexpr GLenum BLEND = GL_BLEND;
  constexpr GLenum BYTE = GL_BYTE;
  constexpr GLenum CLAMP_TO_EDGE = GL_CLAMP_TO_EDGE;
  constexpr GLenum COLOR_ATTACHMENT0 = GL_COLOR_ATTACHMENT0;
  constexpr GLenum COLOR_BUFFER_BIT = GL_COLOR_BUFFER_BIT;
  constexpr GLenum COMPILE_STATUS = GL_COMPILE_STATUS;
  constexpr GLenum CONSTANT_ALPHA = GL_CONSTANT_ALPHA;
  constexpr GLenum CULL_FACE = GL_CULL_FACE;
  constexpr GLenum DEPTH_BUFFER_BIT = GL_DEPTH_BUFFER_BIT;
  constexpr GLenum DEPTH_COMPONENT = GL_DEPTH_COMPONENT;
  constexpr GLenum DEPTH_TEST = GL_DEPTH_TEST;
  constexpr GLenum DST_ALPHA = GL_DST_ALPHA;
  constexpr GLenum DYNAMIC_DRAW = GL_DYNAMIC_DRAW;
  constexpr GLenum ELEMENT_ARRAY_BUFFER = GL_ELEMENT_ARRAY_BUFFER;
  constexpr GLenum FALSE_ = GL_FALSE;
  constexpr GLenum FILL = 0x1B02;
  constexpr GLenum FLOAT = GL_FLOAT;
  constexpr GLenum FRAGMENT_SHADER = GL_FRAGMENT_SHADER;
  constexpr GLenum FRAMEBUFFER = GL_FRAMEBUFFER;
  constexpr GLenum FRAMEBUFFER_COMPLETE = GL_FRAMEBUFFER_COMPLETE;
  constexpr GLenum FRAMEBUFFER_INCOMPLETE_ATTACHMENT = GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
  constexpr GLenum FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER = 0x8CDB;
  constexpr GLenum FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT = GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
  constexpr GLenum FRAMEBUFFER_UNDEFINED = GL_FRAMEBUFFER_UNDEFINED;
  constexpr GLenum FRAMEBUFFER_UNSUPPORTED = GL_FRAMEBUFFER_UNSUPPORTED;
  constexpr GLenum FRONT = GL_FRONT;
  constexpr GLenum FRONT_AND_BACK = GL_FRONT_AND_BACK;
  constexpr GLenum FUNC_ADD = GL_FUNC_ADD;
  constexpr GLenum GEOMETRY_SHADER = 0x8DD9;
  constexpr GLenum INFO_LOG_LENGTH = GL_INFO_LOG_LENGTH;
  constexpr GLenum INT = GL_INT;
  constexpr GLenum INVALID_ENUM = GL_INVALID_ENUM;
  constexpr GLenum INVALID_FRAMEBUFFER_OPERATION = GL_INVALID_FRAMEBUFFER_OPERATION;
  constexpr GLenum INVALID_OPERATION = GL_INVALID_OPERATION;
  constexpr GLenum INVALID_VALUE = GL_INVALID_VALUE;
  constexpr GLenum LINE = 0x1B01;
  constexpr GLenum LINEAR = GL_LINEAR;
  constexpr GLenum LINES = GL_LINES;
  constexpr GLenum LINE_LOOP = GL_LINE_LOOP;
  constexpr GLenum LINE_SMOOTH = 0x0B20;
  constexpr GLenum LINE_STRIP = GL_LINE_STRIP;
  constexpr GLenum LINK_STATUS = GL_LINK_STATUS;
  constexpr GLenum MAJOR_VERSION = GL_MAJOR_VERSION;
  constexpr GLenum MAX_3D_TEXTURE_SIZE = GL_MAX_3D_TEXTURE_SIZE;
  constexpr GLenum MAX_TEXTURE_SIZE = GL_MAX_TEXTURE_SIZE;
  constexpr GLenum MINOR_VERSION = GL_MINOR_VERSION;
  constexpr GLenum MULTISAMPLE = 0x809D;
  constexpr GLenum NEAREST = GL_NEAREST;
  constexpr GLenum ONE = GL_ONE;
  constexpr GLenum ONE_MINUS_CONSTANT_ALPHA = GL_ONE_MINUS_CONSTANT_ALPHA;
  constexpr GLenum ONE_MINUS_SRC_ALPHA = GL_ONE_MINUS_SRC_ALPHA;
  constexpr GLenum OUT_OF_MEMORY = GL_OUT_OF_MEMORY;
  constexpr GLenum PACK_ALIGNMENT = GL_PACK_ALIGNMENT;
  constexpr GLenum POINTS = GL_POINTS;
  constexpr GLenum PRIMITIVE_RESTART = 0x8F9D;
  constexpr GLenum R16F = GL_R16F;
  constexpr GLenum R32F = GL_R32F;
  constexpr GLenum R8 = GL_R8;
  constexpr GLenum R8UI = GL_R8UI;
  constexpr GLenum RED = GL_RED;
  constexpr GLenum RED_INTEGER = GL_RED_INTEGER;
  constexpr GLenum RENDERER = GL_RENDERER;
  constexpr GLenum RG = GL_RG;
  constexpr GLenum RG32F = GL_RG32F;
  constexpr GLenum RGB = GL_RGB;
  constexpr GLenum RGB16F = GL_RGB16F;
  constexpr GLenum RGB32F = GL_RGB32F;
  constexpr GLenum RGBA = GL_RGBA;
  constexpr GLenum RGBA32F = GL_RGBA32F;
  constexpr GLenum SHORT = GL_SHORT;
  constexpr GLenum SMOOTH_LINE_WIDTH_RANGE = 0x0B22;
  constexpr GLenum SRC_ALPHA = GL_SRC_ALPHA;
  constexpr GLenum STATIC_DRAW = GL_STATIC_DRAW;
  constexpr GLenum STREAM_DRAW = GL_STREAM_DRAW;
  constexpr GLenum TEXTURE0 = GL_TEXTURE0;
  constexpr GLenum TEXTURE1 = GL_TEXTURE1;
  constexpr GLenum TEXTURE2 = GL_TEXTURE2;
  constexpr GLenum TEXTURE_2D = GL_TEXTURE_2D;
  constexpr GLenum TEXTURE_3D = GL_TEXTURE_3D;
  constexpr GLenum TEXTURE_BASE_LEVEL = GL_TEXTURE_BASE_LEVEL;
  constexpr GLenum TEXTURE_MAG_FILTER = GL_TEXTURE_MAG_FILTER;
  constexpr GLenum TEXTURE_MAX_LEVEL = GL_TEXTURE_MAX_LEVEL;
  constexpr GLenum TEXTURE_MIN_FILTER = GL_TEXTURE_MIN_FILTER;
  constexpr GLenum TEXTURE_WRAP_R = GL_TEXTURE_WRAP_R;
  constexpr GLenum TEXTURE_WRAP_S = GL_TEXTURE_WRAP_S;
  constexpr GLenum TEXTURE_WRAP_T = GL_TEXTURE_WRAP_T;
  constexpr GLenum TRIANGLES = GL_TRIANGLES;
  constexpr GLenum TRIANGLE_FAN = GL_TRIANGLE_FAN;
  constexpr GLenum TRIANGLE_STRIP = GL_TRIANGLE_STRIP;
  constexpr GLenum TRUE_ = GL_TRUE;
  constexpr GLenum UNPACK_ALIGNMENT = GL_UNPACK_ALIGNMENT;
  constexpr GLenum UNSIGNED_BYTE = GL_UNSIGNED_BYTE;
  constexpr GLenum UNSIGNED_INT = GL_UNSIGNED_INT;
  constexpr GLenum UNSIGNED_SHORT = GL_UNSIGNED_SHORT;
  constexpr GLenum VENDOR = GL_VENDOR;
  constexpr GLenum VERSION = GL_VERSION;
  constexpr GLenum VERTEX_SHADER = GL_VERTEX_SHADER;
  // ---- functions ----
  constexpr auto ActiveTexture = ::glActiveTexture;
  constexpr auto AttachShader = ::glAttachShader;
  constexpr auto BindBuffer = ::glBindBuffer;
  constexpr auto BindFramebuffer = ::glBindFramebuffer;
  constexpr auto BindTexture = ::glBindTexture;
  constexpr auto BindVertexArray = ::glBindVertexArray;
  constexpr auto BlendColor = ::glBlendColor;
  constexpr auto BlendEquation = ::glBlendEquation;
  constexpr auto BlendFunc = ::glBlendFunc;
  constexpr auto BlendFuncSeparate = ::glBlendFuncSeparate;
  constexpr auto BufferData = ::glBufferData;
  constexpr auto CheckFramebufferStatus = ::glCheckFramebufferStatus;
  constexpr auto Clear = ::glClear;
  constexpr auto ClearColor = ::glClearColor;
  constexpr auto ColorMask = ::glColorMask;
  constexpr auto CompileShader = ::glCompileShader;
  constexpr auto CopyTexImage2D = ::glCopyTexImage2D;
  constexpr auto CreateProgram = ::glCreateProgram;
  constexpr auto CreateShader = ::glCreateShader;
  constexpr auto CullFace = ::glCullFace;
  constexpr auto DeleteBuffers = ::glDeleteBuffers;
  constexpr auto DeleteFramebuffers = ::glDeleteFramebuffers;
  constexpr auto DeleteProgram = ::glDeleteProgram;
  constexpr auto DeleteShader = ::glDeleteShader;
  constexpr auto DeleteTextures = ::glDeleteTextures;
  constexpr auto DeleteVertexArrays = ::glDeleteVertexArrays;
  constexpr auto DepthMask = ::glDepthMask;
  constexpr auto DetachShader = ::glDetachShader;
  // MULTISAMPLE/LINE_SMOOTH are not toggleable capabilities in GLES3/WebGL2
  // (raise INVALID_ENUM); multisampling is fixed at context creation. Skip them.
  inline void Disable (GLenum cap) { if (cap == MULTISAMPLE || cap == LINE_SMOOTH) return; ::glDisable (cap); }
  constexpr auto DrawArrays = ::glDrawArrays;
  constexpr auto DrawBuffers = ::glDrawBuffers;
  constexpr auto DrawElements = ::glDrawElements;
  inline void Enable (GLenum cap) { if (cap == MULTISAMPLE || cap == LINE_SMOOTH) return; ::glEnable (cap); }
  constexpr auto EnableVertexAttribArray = ::glEnableVertexAttribArray;
  inline void FramebufferTexture(GLenum t,GLenum a,GLuint tex,GLint l){ ::glFramebufferTexture2D(t,a,GL_TEXTURE_2D,tex,l); }
  constexpr auto GenBuffers = ::glGenBuffers;
  constexpr auto GenFramebuffers = ::glGenFramebuffers;
  constexpr auto GenTextures = ::glGenTextures;
  constexpr auto GenVertexArrays = ::glGenVertexArrays;
  constexpr auto GetBooleanv = ::glGetBooleanv;
  constexpr auto GetError = ::glGetError;
  constexpr auto GetIntegerv = ::glGetIntegerv;
  constexpr auto GetProgramInfoLog = ::glGetProgramInfoLog;
  constexpr auto GetProgramiv = ::glGetProgramiv;
  constexpr auto GetShaderInfoLog = ::glGetShaderInfoLog;
  constexpr auto GetShaderiv = ::glGetShaderiv;
  constexpr auto GetString = ::glGetString;
  inline void GetTexImage(GLenum,GLint,GLenum,GLenum,void*){}
  constexpr auto GetUniformLocation = ::glGetUniformLocation;
  constexpr auto LineWidth = ::glLineWidth;
  constexpr auto LinkProgram = ::glLinkProgram;
  inline void MultiDrawArrays(GLenum m,const GLint*f,const GLsizei*c,GLsizei n){ for(GLsizei i=0;i<n;i++) ::glDrawArrays(m,f[i],c[i]); }
  inline void MultiDrawElements(GLenum m,const GLsizei*c,GLenum t,const void*const*idx,GLsizei n){ for(GLsizei i=0;i<n;i++) ::glDrawElements(m,c[i],t,idx[i]); }
  constexpr auto PixelStorei = ::glPixelStorei;
  inline void PolygonMode(GLenum,GLenum){}
  inline void PrimitiveRestartIndex(GLuint){}
  constexpr auto ReadPixels = ::glReadPixels;
  constexpr auto ShaderSource = ::glShaderSource;
  constexpr auto TexImage2D = ::glTexImage2D;
  constexpr auto TexImage3D = ::glTexImage3D;
  constexpr auto TexParameteri = ::glTexParameteri;
  constexpr auto TexSubImage3D = ::glTexSubImage3D;
  constexpr auto Uniform1f = ::glUniform1f;
  constexpr auto Uniform1i = ::glUniform1i;
  constexpr auto Uniform2iv = ::glUniform2iv;
  constexpr auto Uniform3f = ::glUniform3f;
  constexpr auto Uniform3fv = ::glUniform3fv;
  constexpr auto Uniform3iv = ::glUniform3iv;
  constexpr auto Uniform4fv = ::glUniform4fv;
  constexpr auto UniformMatrix3fv = ::glUniformMatrix3fv;
  constexpr auto UniformMatrix4fv = ::glUniformMatrix4fv;
  constexpr auto UseProgram = ::glUseProgram;
  constexpr auto VertexAttribIPointer = ::glVertexAttribIPointer;
  constexpr auto VertexAttribPointer = ::glVertexAttribPointer;
  constexpr auto Viewport = ::glViewport;
}
#endif
