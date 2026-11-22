// Native OpenGL ES 2.0 / Desktop OpenGL 2.1 renderer for ProjectKagemaru.
#include "gpu_gles.h"
#include "gl_funcs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace arm64 {
namespace {

constexpr size_t kIndexedVertexLimit = 65536;
constexpr size_t kQuadIndices = kIndexedVertexLimit / 4 * 6;

const char *const kVertexShader = R"(
attribute vec3 a_pos;
attribute vec2 a_uv;
attribute vec4 a_color;
uniform vec2 u_inv_screen;
varying highp vec2 v_uv;
varying mediump vec4 v_color;
void main() {
    gl_Position = vec4(a_pos.x * (2.0 * u_inv_screen.x) - 1.0,
                       1.0 - a_pos.y * (2.0 * u_inv_screen.y),
                       2.0 * a_pos.z - 1.0, 1.0);
    v_uv = a_uv;
    v_color = a_color;
}
)";

const char *const kFragmentShader = R"(
varying highp vec2 v_uv;
varying mediump vec4 v_color;
#ifdef TEXTURED
uniform sampler2D s_tex;
#endif
#ifdef CHECKER
uniform highp float u_checker;
#endif
void main() {
#ifdef CHECKER
    highp vec2 p = floor(gl_FragCoord.xy / u_checker);
    highp float s = p.x + p.y;
    if (s - 2.0 * floor(s * 0.5) < 0.5) { discard; }
#endif
#ifdef TEXTURED
    gl_FragColor = texture2D(s_tex, v_uv) * v_color;
#else
    gl_FragColor = v_color;
#endif
}
)";

enum class Prog : uint8_t { Flat, FlatChecker, Ui, Count };
struct ProgramDef { const char *name; const char *defines; };
constexpr ProgramDef kPrograms[int(Prog::Count)] = {
    {"Flat", ""},
    {"FlatChecker", "#define CHECKER\n"},
    {"Ui", "#define TEXTURED\n"},
};

enum class StencilMode : uint8_t {
    Off = 0,
    WriteStage,    // bit 0 = 0 (non-fighter), bit 1 = 0 (unshadowed)
    WriteFighter,  // bit 0 = 1 (fighter),     bit 1 = 0 (unshadowed)
    ShadowFighter, // blend only where (fighter==0 && shadowed==0), then set bit 1
    ShadowAny      // blend only where (shadowed==0), then set bit 1
};

enum class Depth : uint8_t { Off = 0, Test, Write };

struct Vertex {
    float x, y, z;
    float u, v;
    uint32_t color;
};
static_assert(sizeof(Vertex) == 24, "Vertex must stay 24 bytes");

struct Cmd {
    Prog prog;
    GLuint texture;
    uint32_t first, count;
    int clip;
    bool blend;
    bool quads;
    Depth depth;
    StencilMode stencil;
    float checker_px;
};

struct VertexArena {
    std::vector<Vertex> buf;
    size_t size = 0;
    Vertex *append(size_t n) {
        if (size + n > buf.size()) {
            const size_t cap = std::max<size_t>(buf.empty() ? (size_t(1) << 16) : buf.size() * 2, size + n);
            buf.resize(cap);
        }
        Vertex *p = buf.data() + size;
        size += n;
        return p;
    }
    Vertex *data() { return buf.data(); }
    void release() { buf.clear(); buf.shrink_to_fit(); size = 0; }
};

struct State {
    GlFuncs gl;
    SDL_Window *window = nullptr;
    SDL_GLContext ctx = nullptr;
    bool ready = false;
    bool is_gles = false;
    int stencil_bits = 0;
    int max_tex_size = 2048;
    std::string renderer_str;
    int display_w = 720, display_h = 480;

    GLuint program[int(Prog::Count)] = {};
    GLint u_inv_screen[int(Prog::Count)] = {};
    GLint u_checker[int(Prog::Count)] = {};
    static constexpr size_t kVboRing = 3;
    GLuint vbo[kVboRing] = {};
    size_t vbo_idx = 0;
    GLuint quad_ibo = 0;

    VertexArena verts;
    std::vector<Cmd> cmds;
    std::vector<std::array<int, 4>> clips;
    int clip = -1;

    char error[512] = {};
} g;

void set_error(const char *what, const char *detail = nullptr) {
    std::snprintf(g.error, sizeof g.error, "%s%s%s", what, detail && *detail ? ": " : "", detail ? detail : "");
    std::fprintf(stderr, "[ProjectKagemaru GL] %s\n", g.error);
}

// Convert 0x00RRGGBB (BGRA bytes on LE) -> 0xAABBGGRR (RGBA bytes for GL_RGBA)
void swizzle_xrgb_to_rgba(const uint32_t *src, size_t src_stride, uint32_t *dst, int w, int h, bool force_opaque) {
#if defined(__aarch64__)
    static const uint8x16_t kMask = {2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15};
    const uint32x4_t alpha_or = vdupq_n_u32(force_opaque ? 0xff000000u : 0u);
    for (int y = 0; y < h; ++y) {
        const uint32_t *s = src + size_t(y) * src_stride;
        uint32_t *d = dst + size_t(y) * w;
        int x = 0;
        for (; x + 4 <= w; x += 4) {
            uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t *>(s + x));
            v = vqtbl1q_u8(v, kMask);
            if (force_opaque) v = vreinterpretq_u8_u32(vorrq_u32(vreinterpretq_u32_u8(v), alpha_or));
            vst1q_u8(reinterpret_cast<uint8_t *>(d + x), v);
        }
        for (; x < w; ++x) {
            const uint32_t c = s[x];
            const uint32_t a = force_opaque ? 0xff000000u : (c & 0xff000000u);
            d[x] = a | ((c & 0xffu) << 16) | (c & 0xff00u) | ((c >> 16) & 0xffu);
        }
    }
#else
    for (int y = 0; y < h; ++y) {
        const uint32_t *s = src + size_t(y) * src_stride;
        uint32_t *d = dst + size_t(y) * w;
        for (int x = 0; x < w; ++x) {
            const uint32_t c = s[x];
            const uint32_t a = force_opaque ? 0xff000000u : (c & 0xff000000u);
            d[x] = a | ((c & 0xffu) << 16) | (c & 0xff00u) | ((c >> 16) & 0xffu);
        }
    }
#endif
}

inline uint32_t xrgb_to_rgba(uint32_t c, uint32_t alpha = 0xffu) {
    return (alpha << 24) | ((c & 0xffu) << 16) | (c & 0xff00u) | ((c >> 16) & 0xffu);
}

void push(Prog prog, GLuint texture, const Vertex *vertices, size_t count,
          bool blend = true, StencilMode stencil = StencilMode::Off, float checker_px = 1.0f, Depth depth = Depth::Off) {
    if (!count) return;
    const uint32_t first = uint32_t(g.verts.size);
    Vertex *dst = g.verts.append(count);
    std::memcpy(dst, vertices, count * sizeof(Vertex));
    if (!g.cmds.empty()) {
        Cmd &last = g.cmds.back();
        if (!last.quads && last.prog == prog && last.texture == texture &&
            last.clip == g.clip && last.blend == blend && last.depth == depth &&
            last.stencil == stencil && last.checker_px == checker_px && last.first + last.count == first) {
            last.count += uint32_t(count);
            return;
        }
    }
    g.cmds.push_back({prog, texture, first, uint32_t(count), g.clip, blend, false, depth, stencil, checker_px});
}

void push_quad(Prog prog, GLuint texture, float x0, float y0, float x1, float y1,
               float u0, float v0, float u1, float v1, uint32_t color,
               bool blend = true, StencilMode stencil = StencilMode::Off, float checker_px = 1.0f, Depth depth = Depth::Off) {
    const Vertex q[6] = {
        {x0, y0, 0.0f, u0, v0, color}, {x1, y0, 0.0f, u1, v0, color}, {x1, y1, 0.0f, u1, v1, color},
        {x0, y0, 0.0f, u0, v0, color}, {x1, y1, 0.0f, u1, v1, color}, {x0, y1, 0.0f, u0, v1, color},
    };
    push(prog, texture, q, 6, blend, stencil, checker_px, depth);
}

int add_clip(const std::array<int, 4> &rect) {
    for (size_t i = 0; i < g.clips.size(); ++i)
        if (g.clips[i] == rect) return int(i);
    g.clips.push_back(rect);
    return int(g.clips.size() - 1);
}

Vertex *append_quads(size_t quads) {
    if (!g.quad_ibo || !quads) return nullptr;
    const size_t pad = (4 - g.verts.size % 4) % 4;
    if (g.verts.size + pad + quads * 4 > kIndexedVertexLimit) return nullptr;
    g.verts.append(pad + quads * 4);
    return g.verts.data() + (g.verts.size - quads * 4);
}

void apply_stencil(GlFuncs &gl, StencilMode mode, int &cur_mode) {
    const int m = int(mode);
    if (m == cur_mode) return;
    cur_mode = m;
    if (g.stencil_bits < 2 || mode == StencilMode::Off) {
        gl.glDisable(GL_STENCIL_TEST);
        return;
    }
    gl.glEnable(GL_STENCIL_TEST);
    switch (mode) {
    case StencilMode::WriteStage:
        gl.glStencilFunc(GL_ALWAYS, 0x00, 0xFFu);
        gl.glStencilMask(0x03u);
        gl.glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        break;
    case StencilMode::WriteFighter:
        gl.glStencilFunc(GL_ALWAYS, 0x01, 0xFFu);
        gl.glStencilMask(0x03u);
        gl.glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        break;
    case StencilMode::ShadowFighter:
        gl.glStencilFunc(GL_EQUAL, 0x00, 0x03u);
        gl.glStencilMask(0x02u);
        gl.glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
        break;
    case StencilMode::ShadowAny:
        gl.glStencilFunc(GL_EQUAL, 0x00, 0x02u);
        gl.glStencilMask(0x02u);
        gl.glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
        break;
    case StencilMode::Off:
        break;
    }
}

void execute() {
    if (g.cmds.empty() || !g.verts.size) return;
    auto &gl = g.gl;
    const GLuint cur_vbo = g.vbo[g.vbo_idx];
    g.vbo_idx = (g.vbo_idx + 1) % State::kVboRing;
    gl.glBindBuffer(GL_ARRAY_BUFFER, cur_vbo);
    gl.glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(g.verts.size * sizeof(Vertex)), nullptr, GL_STREAM_DRAW);
    gl.glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(g.verts.size * sizeof(Vertex)), g.verts.data(), GL_STREAM_DRAW);
    if (g.quad_ibo) gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.quad_ibo);
    for (GLuint i = 0; i < 3; ++i) gl.glEnableVertexAttribArray(i);
    gl.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void *>(0));
    gl.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void *>(12));
    gl.glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), reinterpret_cast<const void *>(20));

    gl.glActiveTexture(GL_TEXTURE0);
    const float inv_w = 1.0f / float(std::max(g.display_w, 1));
    const float inv_h = 1.0f / float(std::max(g.display_h, 1));

    GLuint program = 0, texture = 0;
    int blend = -1, depth_mode = -1, clip = -2, stencil = -1;
    float last_checker = -1.0f;
    for (const Cmd &c : g.cmds) {
        const int pi = int(c.prog);
        if (g.program[pi] != program) {
            program = g.program[pi];
            gl.glUseProgram(program);
            if (g.u_inv_screen[pi] >= 0) gl.glUniform2f(g.u_inv_screen[pi], inv_w, inv_h);
            last_checker = -1.0f;
        }
        if (c.prog == Prog::FlatChecker && g.u_checker[pi] >= 0 && c.checker_px != last_checker) {
            gl.glUniform1f(g.u_checker[pi], std::max(c.checker_px, 1.0f));
            last_checker = c.checker_px;
        }
        const int dm = int(c.depth);
        if (dm != depth_mode) {
            if (c.depth == Depth::Off) {
                gl.glDisable(GL_DEPTH_TEST);
                gl.glDepthMask(GL_FALSE);
            } else {
                if (depth_mode <= 0) gl.glEnable(GL_DEPTH_TEST);
                gl.glDepthMask(c.depth == Depth::Write ? GL_TRUE : GL_FALSE);
            }
            depth_mode = dm;
        }
        const int want_blend = c.blend ? 1 : 0;
        if (want_blend != blend) {
            if (want_blend) gl.glEnable(GL_BLEND);
            else gl.glDisable(GL_BLEND);
            blend = want_blend;
        }
        apply_stencil(gl, c.stencil, stencil);
        if (c.prog == Prog::Ui && c.texture != texture) {
            gl.glBindTexture(GL_TEXTURE_2D, c.texture);
            texture = c.texture;
        }
        if (c.clip != clip) {
            if (c.clip < 0) {
                gl.glDisable(GL_SCISSOR_TEST);
            } else {
                const std::array<int, 4> &r = g.clips[size_t(c.clip)];
                gl.glEnable(GL_SCISSOR_TEST);
                gl.glScissor(r[0], g.display_h - r[3], r[2] - r[0], r[3] - r[1]);
            }
            clip = c.clip;
        }
        if (c.quads) {
            const size_t offset = size_t(c.first / 4) * 6 * sizeof(uint16_t);
            gl.glDrawElements(GL_TRIANGLES, GLsizei(c.count / 4 * 6), GL_UNSIGNED_SHORT,
                              reinterpret_cast<const void *>(offset));
        } else {
            gl.glDrawArrays(GL_TRIANGLES, GLint(c.first), GLsizei(c.count));
        }
    }
    gl.glDisable(GL_SCISSOR_TEST);
    gl.glDisable(GL_STENCIL_TEST);
    gl.glStencilMask(0xFFu);
    if (g.quad_ibo) gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    gl.glBindBuffer(GL_ARRAY_BUFFER, 0);
}

GLuint compile_shader(GLenum type, const char *header, const char *defines, const char *body, const char *name) {
    auto &gl = g.gl;
    const GLuint shader = gl.glCreateShader(type);
    const GLchar *sources[] = {header, defines, body};
    gl.glShaderSource(shader, 3, sources, nullptr);
    gl.glCompileShader(shader);
    GLint ok = GL_FALSE;
    gl.glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE) return shader;
    char log[400] = {};
    gl.glGetShaderInfoLog(shader, sizeof log, nullptr, log);
    set_error(name, log);
    gl.glDeleteShader(shader);
    return 0;
}

GLuint build_program(const ProgramDef &def, GLint &u_inv_out, GLint &u_chk_out) {
    const char *v_hdr = g.is_gles ? "#version 100\nprecision highp float;\n"
                                  : "#version 120\n#define highp\n#define mediump\n#define lowp\n";
    const char *f_hdr = g.is_gles ? "#version 100\nprecision mediump float;\n"
                                  : "#version 120\n#define highp\n#define mediump\n#define lowp\n";
    const GLuint vs = compile_shader(GL_VERTEX_SHADER, v_hdr, def.defines, kVertexShader, def.name);
    if (!vs) return 0;
    const GLuint fs = compile_shader(GL_FRAGMENT_SHADER, f_hdr, def.defines, kFragmentShader, def.name);
    if (!fs) { g.gl.glDeleteShader(vs); return 0; }
    auto &gl = g.gl;
    const GLuint prog = gl.glCreateProgram();
    gl.glAttachShader(prog, vs);
    gl.glAttachShader(prog, fs);
    gl.glBindAttribLocation(prog, 0, "a_pos");
    gl.glBindAttribLocation(prog, 1, "a_uv");
    gl.glBindAttribLocation(prog, 2, "a_color");
    gl.glLinkProgram(prog);
    gl.glDeleteShader(vs);
    gl.glDeleteShader(fs);
    GLint ok = GL_FALSE;
    gl.glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[400] = {};
        gl.glGetProgramInfoLog(prog, sizeof log, nullptr, log);
        set_error(def.name, log);
        gl.glDeleteProgram(prog);
        return 0;
    }
    gl.glUseProgram(prog);
    u_inv_out = gl.glGetUniformLocation(prog, "u_inv_screen");
    u_chk_out = gl.glGetUniformLocation(prog, "u_checker");
    const GLint s_tex = gl.glGetUniformLocation(prog, "s_tex");
    if (s_tex >= 0) gl.glUniform1i(s_tex, 0);
    return prog;
}

void compute_viewport(int w2d, AspectMode aspect, float &sx, float &sy, float &ox, float &oy) {
    const float dw = float(std::max(g.display_w, 1));
    const float dh = float(std::max(g.display_h, 1));
    const float sw = float(std::max(w2d, 1));
    const float sh = float(model1::SCREEN_H);
    if (aspect == AspectMode::StretchFull) {
        sx = dw / sw;
        sy = dh / sh;
        ox = 0.0f;
        oy = 0.0f;
    } else {
        const float s = std::min(dw / sw, dh / sh);
        sx = s;
        sy = s;
        ox = (dw - sw * s) * 0.5f;
        oy = (dh - sh * s) * 0.5f;
    }
}

void upload_dirty_rows(GlFuncs &gl, const uint8_t *row_dirty, int w, int h,
                       GLenum format, GLenum type, const uint16_t *shadow) {
    int y = 0;
    while (y < h) {
        while (y < h && !row_dirty[y]) ++y;
        if (y >= h) break;
        const int y0 = y;
        int y1 = y + 1;
        while (y1 < h) {
            if (row_dirty[y1]) { ++y1; continue; }
            int gap = 0;
            while (y1 + gap < h && !row_dirty[y1 + gap] && gap < 8) ++gap;
            if (y1 + gap < h && row_dirty[y1 + gap]) { y1 += gap + 1; continue; }
            break;
        }
        gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y0, w, y1 - y0, format, type,
                           shadow + size_t(y0) * size_t(w));
        y = y1;
    }
}

} // namespace

bool gl_init(const GlConfig &cfg) {
    gl_fini();
    g.error[0] = 0;

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (cfg.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    struct TryProfile { int profile; int maj; int min; int depth; int stencil; };
    const TryProfile profiles[] = {
        {SDL_GL_CONTEXT_PROFILE_ES, 2, 0, 24, 8},
        {SDL_GL_CONTEXT_PROFILE_ES, 2, 0, 16, 8},
        {SDL_GL_CONTEXT_PROFILE_ES, 2, 0, 16, 0},
        {SDL_GL_CONTEXT_PROFILE_COMPATIBILITY, 2, 1, 24, 8},
        {SDL_GL_CONTEXT_PROFILE_COMPATIBILITY, 2, 1, 16, 0},
    };
    for (const auto &p : profiles) {
        SDL_GL_ResetAttributes();
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, p.depth);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, p.stencil);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, p.profile);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, p.maj);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, p.min);
        g.window = SDL_CreateWindow("ProjectKagemaru — Virtua Fighter", SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED, cfg.window_w, cfg.window_h, flags);
        if (!g.window) continue;
        g.ctx = SDL_GL_CreateContext(g.window);
        if (g.ctx) break;
        SDL_DestroyWindow(g.window);
        g.window = nullptr;
    }
    if (!g.window || !g.ctx) {
        set_error("SDL_GL_CreateContext failed", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(g.window, g.ctx);
    if (!g.gl.load()) {
        set_error("Failed to load GLES 2.0 / GL 2.1 functions");
        gl_fini();
        return false;
    }
    auto &gl = g.gl;
    const char *ver = reinterpret_cast<const char *>(gl.glGetString(GL_VERSION));
    const char *ren = reinterpret_cast<const char *>(gl.glGetString(GL_RENDERER));
    g.is_gles = (ver && std::strstr(ver, "OpenGL ES") != nullptr);
    GLint sbits = 0, tmax = 2048;
    gl.glGetIntegerv(GL_STENCIL_BITS, &sbits);
    gl.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &tmax);
    g.stencil_bits = sbits;
    g.max_tex_size = std::max(tmax, 512);
    g.renderer_str = std::string(g.is_gles ? "GLES2: " : "GL2: ") + (ren ? ren : "unknown") +
                     " (" + (ver ? ver : "?") + ", S" + std::to_string(g.stencil_bits) + ")";
    std::fprintf(stderr, "[ProjectKagemaru] Graphics: %s\n", g.renderer_str.c_str());

    SDL_ShowCursor(SDL_DISABLE);
    gl_set_vsync(cfg.vsync);
    gl_update_drawable_size();

    gl.glViewport(0, 0, g.display_w, g.display_h);
    gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl.clear_depth(0.0f);
    gl.depth_range(0.0f, 1.0f);
    gl.glClearStencil(0);
    gl.glDepthFunc(GL_GREATER);
    gl.glDepthMask(GL_FALSE);
    gl.glDisable(GL_DEPTH_TEST);
    gl.glDisable(GL_CULL_FACE);
    gl.glDisable(GL_SCISSOR_TEST);
    gl.glDisable(GL_STENCIL_TEST);
    gl.glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (int i = 0; i < int(Prog::Count); ++i)
        g.program[i] = build_program(kPrograms[i], g.u_inv_screen[i], g.u_checker[i]);
    if (!g.program[int(Prog::Flat)] || !g.program[int(Prog::Ui)]) {
        gl_fini();
        return false;
    }
    if (!g.program[int(Prog::FlatChecker)]) {
        g.program[int(Prog::FlatChecker)] = g.program[int(Prog::Flat)];
        g.u_inv_screen[int(Prog::FlatChecker)] = g.u_inv_screen[int(Prog::Flat)];
    }

    gl.glGenBuffers(State::kVboRing, g.vbo);
    if (!g.vbo[0]) {
        set_error("VBO creation failed");
        gl_fini();
        return false;
    }
    g.verts.buf.resize(size_t(1) << 16);
    g.cmds.reserve(1024);

    std::vector<uint16_t> indices(kQuadIndices);
    for (size_t q = 0; q < kIndexedVertexLimit / 4; ++q) {
        const uint16_t v = uint16_t(q * 4);
        uint16_t *idx = &indices[q * 6];
        idx[0] = v; idx[1] = uint16_t(v + 1); idx[2] = uint16_t(v + 2);
        idx[3] = v; idx[4] = uint16_t(v + 2); idx[5] = uint16_t(v + 3);
    }
    gl.glGenBuffers(1, &g.quad_ibo);
    if (g.quad_ibo) {
        gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.quad_ibo);
        gl.glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(indices.size() * sizeof(uint16_t)), indices.data(),
                        GL_STATIC_DRAW);
        gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }

    g.ready = true;
    return true;
}

const char *gl_error() { return g.error; }
const char *gl_renderer_string() { return g.renderer_str.c_str(); }
bool gl_is_gles() { return g.is_gles; }
int gl_stencil_bits() { return g.stencil_bits; }
SDL_Window *gl_window() { return g.window; }
int gl_display_width() { return g.display_w; }
int gl_display_height() { return g.display_h; }

void gl_update_drawable_size() {
    if (!g.window) return;
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(g.window, &w, &h);
    if (w <= 0 || h <= 0) SDL_GetWindowSize(g.window, &w, &h);
    g.display_w = std::max(w, 1);
    g.display_h = std::max(h, 1);
}

void gl_set_vsync(bool enabled) {
    if (SDL_GL_SetSwapInterval(enabled ? 1 : 0) < 0 && enabled)
        SDL_GL_SetSwapInterval(0);
}

void gl_begin_frame() {
    g.verts.size = 0;
    g.cmds.clear();
    g.clips.clear();
    g.clip = -1;
    auto &gl = g.gl;
    gl.glViewport(0, 0, g.display_w, g.display_h);
    gl.glDisable(GL_SCISSOR_TEST);
    gl.glDisable(GL_DEPTH_TEST);
    gl.glDisable(GL_STENCIL_TEST);
    gl.glDepthMask(GL_TRUE);
    gl.glStencilMask(0xFFu);
    gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    gl.glDepthMask(GL_FALSE);
}

void gl_end_frame() {
    execute();
    if (g.window) SDL_GL_SwapWindow(g.window);
}

void gl_fini() {
    if (g.ctx) {
        auto &gl = g.gl;
        if (gl.glFinish) gl.glFinish();
        for (int i = 0; i < int(Prog::Count); ++i) {
            bool shared = false;
            for (int j = 0; j < i; ++j) shared = shared || g.program[j] == g.program[i];
            if (g.program[i] && !shared && gl.glDeleteProgram) gl.glDeleteProgram(g.program[i]);
            g.program[i] = 0;
        }
        if (g.vbo[0] && gl.glDeleteBuffers) gl.glDeleteBuffers(State::kVboRing, g.vbo);
        if (g.quad_ibo && gl.glDeleteBuffers) gl.glDeleteBuffers(1, &g.quad_ibo);
        for (auto &v : g.vbo) v = 0;
        g.quad_ibo = 0;
        SDL_GL_DeleteContext(g.ctx);
        g.ctx = nullptr;
    }
    if (g.window) {
        SDL_DestroyWindow(g.window);
        g.window = nullptr;
    }
    g.verts.release();
    g.ready = false;
}

void gl_fill_rect(float x, float y, float w, float h, uint32_t rgba) {
    push_quad(Prog::Flat, 0, x, y, x + w, y + h, 0.0f, 0.0f, 0.0f, 0.0f, rgba, true);
}

uint32_t gl_create_ui_texture(int w, int h, const void *rgba) {
    if (w <= 0 || h <= 0 || !rgba || !g.ready) return 0;
    auto &gl = g.gl;
    GLuint tex = 0;
    gl.glGenTextures(1, &tex);
    if (!tex) return 0;
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindTexture(GL_TEXTURE_2D, tex);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return tex;
}

void gl_delete_ui_texture(uint32_t texture) {
    if (!texture || !g.ready) return;
    GLuint t = texture;
    g.gl.glDeleteTextures(1, &t);
}

void gl_ui_triangles(uint32_t texture, const GlUiVertex *vertices, size_t count, int x0, int y0, int x1, int y1) {
    count -= count % 3;
    if (!texture || !count) return;
    x0 = std::clamp(x0, 0, g.display_w);
    x1 = std::clamp(x1, 0, g.display_w);
    y0 = std::clamp(y0, 0, g.display_h);
    y1 = std::clamp(y1, 0, g.display_h);
    if (x1 <= x0 || y1 <= y0) return;
    Vertex *dst = g.verts.append(count);
    const uint32_t first = uint32_t(dst - g.verts.data());
    for (size_t i = 0; i < count; ++i) {
        const GlUiVertex &v = vertices[i];
        dst[i] = {v.x, v.y, 0.0f, v.u, v.v, v.rgba};
    }
    const int clip = add_clip({x0, y0, x1, y1});
    if (!g.cmds.empty()) {
        Cmd &last = g.cmds.back();
        if (!last.quads && last.prog == Prog::Ui && last.texture == texture && last.clip == clip &&
            last.blend && last.depth == Depth::Off && last.stencil == StencilMode::Off &&
            last.first + last.count == first) {
            last.count += uint32_t(count);
            return;
        }
    }
    g.cmds.push_back({Prog::Ui, texture, first, uint32_t(count), clip, true, false, Depth::Off, StencilMode::Off, 1.0f});
}

// ---- GpuGlesRenderer ---------------------------------------------------------

GpuGlesRenderer::GpuGlesRenderer() { ok_ = g.ready; }
GpuGlesRenderer::~GpuGlesRenderer() { shutdown(); }

uint32_t GpuGlesRenderer::make_texture(uint32_t w, uint32_t h, bool linear, uint32_t wrap_s, uint32_t wrap_t,
                                       uint32_t format, const void *pixels) {
    auto &gl = g.gl;
    GLuint tex = 0;
    gl.glGenTextures(1, &tex);
    if (!tex) return 0;
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindTexture(GL_TEXTURE_2D, tex);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GLint(wrap_s));
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GLint(wrap_t));
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GLint(format), GLsizei(w), GLsizei(h), 0, GLenum(format),
                    GL_UNSIGNED_BYTE, pixels);
    return tex;
}

void GpuGlesRenderer::update_texture_rgba(uint32_t &tex, int &cur_w, int &cur_h, int w, int h, bool linear,
                                          const uint32_t *xrgb, size_t stride_px) {
    if (w <= 0 || h <= 0 || !xrgb) return;
    swizzle_scratch_.resize(size_t(w) * size_t(h));
    swizzle_xrgb_to_rgba(xrgb, stride_px, swizzle_scratch_.data(), w, h, true);
    auto &gl = g.gl;
    if (!tex || cur_w != w || cur_h != h) {
        if (tex && g.ready) { GLuint t = tex; gl.glDeleteTextures(1, &t); }
        tex = make_texture(uint32_t(w), uint32_t(h), linear, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE,
                           GL_RGBA, swizzle_scratch_.data());
        cur_w = w;
        cur_h = h;
    } else {
        gl.glActiveTexture(GL_TEXTURE0);
        gl.glBindTexture(GL_TEXTURE_2D, tex);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, swizzle_scratch_.data());
    }
}

void GpuGlesRenderer::update_tex_rgb565(LayerTex2D &layer, const model1::gpu_image &im, bool linear) {
    if (!im.px || im.w <= 0 || im.h <= 0) return;
    if (layer.tex[0] && layer.last_gen == im.gen && layer.w == im.w / std::max(layer.stride_scale, 1) && layer.h == im.h) {
        if (layer.linear != linear) {
            auto &gl = g.gl;
            gl.glActiveTexture(GL_TEXTURE0);
            for (int i = 0; i < 2; ++i) {
                gl.glBindTexture(GL_TEXTURE_2D, layer.tex[i]);
                gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
                gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
            }
            layer.linear = linear;
        }
        return;
    }

    int step = 1;
    while ((im.w / step) > g.max_tex_size) step *= 2;
    const int w = std::max(1, im.w / step);
    const int h = im.h;
    const size_t total = size_t(w) * size_t(h);
    const bool realloc_tex = (!layer.tex[0] || !layer.tex[1] || layer.w != w || layer.h != h ||
                              layer.shadow[0].size() != total || layer.shadow[1].size() != total);
    if (realloc_tex) {
        auto &gl = g.gl;
        for (int i = 0; i < 2; ++i) {
            if (layer.tex[i] && g.ready) { GLuint t = layer.tex[i]; gl.glDeleteTextures(1, &t); layer.tex[i] = 0; }
            layer.shadow[i].assign(total, 0u);
        }
        layer.w = w;
        layer.h = h;
        layer.stride_scale = step;
        layer.linear = linear;
        layer.cur = 0;
    } else {
        layer.cur ^= 1u;
    }
    layer.last_gen = im.gen;

    uint32_t &tex = layer.tex[layer.cur];
    std::vector<uint16_t> &shadow = layer.shadow[layer.cur];
    std::vector<uint8_t> row_dirty(size_t(h), 0);

    for (int y = 0; y < h; ++y) {
        const uint32_t *src = im.px + size_t(y) * im.stride;
        uint16_t *dst = shadow.data() + size_t(y) * size_t(w);
        uint32_t diff_acc = realloc_tex ? 1u : 0u;
        int x = 0;
        if (step == 1) {
#if defined(__aarch64__)
            uint16x8_t vdiff = vdupq_n_u16(0);
            for (; x + 8 <= w; x += 8) {
                const uint32x4_t c0 = vld1q_u32(src + x);
                const uint32x4_t c1 = vld1q_u32(src + x + 4);
                const uint16x4_t p0 = vorr_u16(
                    vorr_u16(vand_u16(vshrn_n_u32(c0, 8), vdup_n_u16(0xf800u)),
                             vand_u16(vshrn_n_u32(c0, 5), vdup_n_u16(0x07e0u))),
                    vand_u16(vshr_n_u16(vmovn_u32(c0), 3), vdup_n_u16(0x001fu)));
                const uint16x4_t p1 = vorr_u16(
                    vorr_u16(vand_u16(vshrn_n_u32(c1, 8), vdup_n_u16(0xf800u)),
                             vand_u16(vshrn_n_u32(c1, 5), vdup_n_u16(0x07e0u))),
                    vand_u16(vshr_n_u16(vmovn_u32(c1), 3), vdup_n_u16(0x001fu)));
                const uint16x8_t p = vcombine_u16(p0, p1);
                const uint16x8_t old = vld1q_u16(dst + x);
                vdiff = vorrq_u16(vdiff, veorq_u16(old, p));
                vst1q_u16(dst + x, p);
            }
            diff_acc |= vmaxvq_u16(vdiff);
#endif
            for (; x < w; ++x) {
                const uint32_t c = src[x];
                const uint16_t p = uint16_t(((c >> 8) & 0xf800u) | ((c >> 5) & 0x07e0u) | ((c >> 3) & 0x001fu));
                diff_acc |= uint32_t(dst[x] ^ p);
                dst[x] = p;
            }
        } else {
            for (; x < w; ++x) {
                const uint32_t c = src[x * step];
                const uint16_t p = uint16_t(((c >> 8) & 0xf800u) | ((c >> 5) & 0x07e0u) | ((c >> 3) & 0x001fu));
                diff_acc |= uint32_t(dst[x] ^ p);
                dst[x] = p;
            }
        }
        if (diff_acc) row_dirty[size_t(y)] = 1;
    }

    auto &gl = g.gl;
    if (realloc_tex) {
        for (int i = 0; i < 2; ++i) {
            GLuint t = 0;
            gl.glGenTextures(1, &t);
            layer.tex[i] = t;
            if (!t) continue;
            gl.glActiveTexture(GL_TEXTURE0);
            gl.glBindTexture(GL_TEXTURE_2D, t);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, shadow.data());
        }
        if (layer.shadow[1].data() != shadow.data()) layer.shadow[1] = shadow;
        return;
    }

    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindTexture(GL_TEXTURE_2D, tex);
    if (layer.linear != linear) {
        for (int i = 0; i < 2; ++i) {
            gl.glBindTexture(GL_TEXTURE_2D, layer.tex[i]);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        }
        gl.glBindTexture(GL_TEXTURE_2D, tex);
        layer.linear = linear;
    }
    upload_dirty_rows(gl, row_dirty.data(), w, h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, shadow.data());
}

void GpuGlesRenderer::update_tex_rgba5551(LayerTex2D &layer, const model1::gpu_image &im, bool linear) {
    if (!im.px || im.w <= 0 || im.h <= 0) return;
    if (layer.tex[0] && layer.last_gen == im.gen && layer.w == im.w && layer.h == im.h) return;

    const int w = im.w;
    const int h = im.h;
    const size_t total = size_t(w) * size_t(h);
    const bool realloc_tex = (!layer.tex[0] || !layer.tex[1] || layer.w != w || layer.h != h ||
                              layer.shadow[0].size() != total || layer.shadow[1].size() != total);
    if (realloc_tex) {
        auto &gl = g.gl;
        for (int i = 0; i < 2; ++i) {
            if (layer.tex[i] && g.ready) { GLuint t = layer.tex[i]; gl.glDeleteTextures(1, &t); layer.tex[i] = 0; }
            layer.shadow[i].assign(total, 0u);
        }
        layer.w = w;
        layer.h = h;
        layer.stride_scale = 1;
        layer.linear = linear;
        layer.cur = 0;
    } else {
        layer.cur ^= 1u;
    }
    layer.last_gen = im.gen;

    uint32_t &tex = layer.tex[layer.cur];
    std::vector<uint16_t> &shadow = layer.shadow[layer.cur];
    std::vector<uint8_t> row_dirty(size_t(h), 0);

    for (int y = 0; y < h; ++y) {
        const uint32_t *src = im.px + size_t(y) * im.stride;
        uint16_t *dst = shadow.data() + size_t(y) * size_t(w);
        uint32_t diff_acc = realloc_tex ? 1u : 0u;
        int x = 0;
#if defined(__aarch64__)
        uint16x8_t vdiff = vdupq_n_u16(0);
        const uint32x4_t vkey = vdupq_n_u32(model1::KEY);
        for (; x + 8 <= w; x += 8) {
            const uint32x4_t c0 = vld1q_u32(src + x);
            const uint32x4_t c1 = vld1q_u32(src + x + 4);
            const uint16x4_t m0 = vmovn_u32(vmvnq_u32(vceqq_u32(c0, vkey)));
            const uint16x4_t m1 = vmovn_u32(vmvnq_u32(vceqq_u32(c1, vkey)));
            const uint16x4_t rgb0 = vorr_u16(
                vorr_u16(vand_u16(vshrn_n_u32(c0, 8), vdup_n_u16(0xf800u)),
                         vand_u16(vshrn_n_u32(c0, 5), vdup_n_u16(0x07c0u))),
                vorr_u16(vand_u16(vshrn_n_u32(c0, 2), vdup_n_u16(0x003eu)), vdup_n_u16(1u)));
            const uint16x4_t rgb1 = vorr_u16(
                vorr_u16(vand_u16(vshrn_n_u32(c1, 8), vdup_n_u16(0xf800u)),
                         vand_u16(vshrn_n_u32(c1, 5), vdup_n_u16(0x07c0u))),
                vorr_u16(vand_u16(vshrn_n_u32(c1, 2), vdup_n_u16(0x003eu)), vdup_n_u16(1u)));
            const uint16x8_t p = vcombine_u16(vand_u16(rgb0, m0), vand_u16(rgb1, m1));
            const uint16x8_t old = vld1q_u16(dst + x);
            vdiff = vorrq_u16(vdiff, veorq_u16(old, p));
            vst1q_u16(dst + x, p);
        }
        diff_acc |= vmaxvq_u16(vdiff);
#endif
        for (; x < w; ++x) {
            const uint32_t c = src[x];
            const uint16_t p = (c != model1::KEY)
                ? uint16_t((((c >> 19) & 0x1fu) << 11) | (((c >> 11) & 0x1fu) << 6) | (((c >> 3) & 0x1fu) << 1) | 1u)
                : 0u;
            diff_acc |= uint32_t(dst[x] ^ p);
            dst[x] = p;
        }
        if (diff_acc) row_dirty[size_t(y)] = 1;
    }

    auto &gl = g.gl;
    if (realloc_tex) {
        for (int i = 0; i < 2; ++i) {
            GLuint t = 0;
            gl.glGenTextures(1, &t);
            layer.tex[i] = t;
            if (!t) continue;
            gl.glActiveTexture(GL_TEXTURE0);
            gl.glBindTexture(GL_TEXTURE_2D, t);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, shadow.data());
        }
        if (layer.shadow[1].data() != shadow.data()) layer.shadow[1] = shadow;
        return;
    }

    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindTexture(GL_TEXTURE_2D, tex);
    upload_dirty_rows(gl, row_dirty.data(), w, h, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, shadow.data());
}

void GpuGlesRenderer::reset_caches() {
    auto &gl = g.gl;
    for (auto &l : layers_) {
        for (int i = 0; i < 2; ++i) {
            if (l.tex[i] && g.ready) { GLuint t = l.tex[i]; gl.glDeleteTextures(1, &t); }
            l.tex[i] = 0;
            l.shadow[i].clear();
        }
        l.w = l.h = 0;
        l.last_gen = UINT64_MAX;
    }
}

void GpuGlesRenderer::shutdown() {
    if (shutdown_) return;
    reset_caches();
    if (tex_cpu_ && g.ready) { GLuint t = tex_cpu_; g.gl.glDeleteTextures(1, &t); }
    tex_cpu_ = 0;
    cpu_w_ = cpu_h_ = 0;
    shutdown_ = true;
    ok_ = false;
}

void GpuGlesRenderer::emit_3d_polys(const gpu3d_poly *polys, size_t count,
                                    float sx, float sy, float ox, float oy,
                                    bool arcade_shadows, int render_scale) {
    last_poly_count_ = count;
    last_batch_count_ = 0;
    if (!count || !polys) return;

    const float inv_rs = 1.0f / float(std::max(render_scale, 1));
    const float psx = sx * inv_rs;
    const float psy = sy * inv_rs;
    const float checker_px = std::max(1.0f, std::round(sx));

    auto flush_run = [&](Prog prog, int clip, bool blend, Depth depth_mode, StencilMode stencil,
                         size_t run_quads, const Vertex *run_src) {
        if (!run_quads) return;
        size_t offset = 0;
        while (offset < run_quads) {
            const size_t chunk = std::min(run_quads - offset, kIndexedVertexLimit / 4);
            Vertex *dst = append_quads(chunk);
            if (dst) {
                const uint32_t first = uint32_t(dst - g.verts.data());
                std::memcpy(dst, run_src + offset * 4, chunk * 4 * sizeof(Vertex));
                g.cmds.push_back({prog, 0, first, uint32_t(chunk * 4), clip, blend, true, depth_mode, stencil, checker_px});
            } else {
                for (size_t q = 0; q < chunk; ++q) {
                    const Vertex *v = run_src + (offset + q) * 4;
                    const Vertex tri[6] = {v[0], v[1], v[2], v[0], v[2], v[3]};
                    g.clip = clip;
                    push(prog, 0, tri, 6, blend, stencil, checker_px, depth_mode);
                }
            }
            ++last_batch_count_;
            offset += chunk;
        }
    };

    struct PolyBucket {
        uint64_t key = 0;
        int clip = 0;
        Prog prog = Prog::Flat;
        bool blend = false;
        Depth depth_mode = Depth::Write;
        StencilMode stencil = StencilMode::Off;
        std::vector<Vertex> verts;
    };
    static std::vector<PolyBucket> buckets;
    for (auto &b : buckets) b.verts.clear();
    size_t used_buckets = 0;

    auto get_bucket = [&](int clip, Prog prog, bool blend, Depth depth_mode, StencilMode stencil) -> PolyBucket & {
        const uint64_t key = (uint64_t(uint16_t(clip)) << 24) |
                             (uint64_t(prog) << 16) |
                             (uint64_t(blend ? 1 : 0) << 12) |
                             (uint64_t(depth_mode) << 8) |
                             uint64_t(stencil);
        for (size_t bi = 0; bi < used_buckets; ++bi) {
            if (buckets[bi].key == key) return buckets[bi];
        }
        if (used_buckets == buckets.size()) buckets.emplace_back();
        PolyBucket &b = buckets[used_buckets++];
        b.key = key;
        b.clip = clip;
        b.prog = prog;
        b.blend = blend;
        b.depth_mode = depth_mode;
        b.stencil = stencil;
        b.verts.clear();
        return b;
    };

    const float depth_step = 0.9998f / float(count + 2);
    for (size_t i = 0; i < count; ++i) {
        const gpu3d_poly &p = polys[i];
        const int vx0 = std::clamp(int(std::floor(ox + float(p.cx0) * psx)), 0, g.display_w);
        const int vy0 = std::clamp(int(std::floor(oy + float(p.cy0) * psy)), 0, g.display_h);
        const int vx1 = std::clamp(int(std::ceil(ox + float(p.cx1 + 1) * psx)), 0, g.display_w);
        const int vy1 = std::clamp(int(std::ceil(oy + float(p.cy1 + 1) * psy)), 0, g.display_h);
        if (vx1 <= vx0 || vy1 <= vy0) continue;
        const int clip = add_clip({vx0, vy0, vx1, vy1});

        const bool is_line = (p.flags & G3_LINE) != 0;
        const bool is_checker = (p.flags & G3_CHECKER) != 0;
        const bool is_shadow = !is_checker && ((p.flags & G3_SHADOW) != 0);
        const bool is_fighter_shadow = is_shadow && ((p.flags & G3_FIGHTER) != 0);
        const bool is_stage = (p.flags & G3_STAGE) != 0;

        const Prog prog = (is_checker && (arcade_shadows || is_line)) ? Prog::FlatChecker : Prog::Flat;
        const bool blend = is_shadow;
        const Depth depth_mode = (is_shadow || prog == Prog::FlatChecker) ? Depth::Test : Depth::Write;
        StencilMode stencil = StencilMode::Off;
        if (g.stencil_bits >= 2 && !arcade_shadows) {
            if (is_fighter_shadow) stencil = StencilMode::ShadowFighter;
            else if (is_shadow) stencil = StencilMode::ShadowAny;
            else if (!is_line) stencil = is_stage ? StencilMode::WriteStage : StencilMode::WriteFighter;
        }
        const uint32_t col = xrgb_to_rgba(p.colour, is_shadow ? 128u : 255u);
        const float z = float(i + 1) * depth_step;

        Vertex qv[4];
        if (is_line) {
            const float x0 = ox + p.x[0] * psx, y0 = oy + p.y[0] * psy;
            const float x1 = ox + p.x[1] * psx, y1 = oy + p.y[1] * psy;
            const float dx = x1 - x0, dy = y1 - y0;
            const float len = std::hypot(dx, dy);
            const float half = std::max(0.5f, 0.5f * psx);
            const float nx = len > 1e-4f ? (-dy / len) * half : half;
            const float ny = len > 1e-4f ? (dx / len) * half : 0.0f;
            qv[0] = {x0 - nx, y0 - ny, z, 0, 0, col};
            qv[1] = {x0 + nx, y0 + ny, z, 0, 0, col};
            qv[2] = {x1 + nx, y1 + ny, z, 0, 0, col};
            qv[3] = {x1 - nx, y1 - ny, z, 0, 0, col};
        } else {
            for (int k = 0; k < 4; ++k) {
                qv[k] = {ox + p.x[k] * psx, oy + p.y[k] * psy, z, 0.0f, 0.0f, col};
            }
        }

        PolyBucket &b = get_bucket(clip, prog, blend, depth_mode, stencil);
        b.verts.insert(b.verts.end(), qv, qv + 4);
    }

    // Pass 1: Opaque geometry (writes depth + stencil)
    for (size_t bi = 0; bi < used_buckets; ++bi) {
        const PolyBucket &b = buckets[bi];
        if (b.depth_mode != Depth::Write || b.verts.empty()) continue;
        flush_run(b.prog, b.clip, b.blend, b.depth_mode, b.stencil, b.verts.size() / 4, b.verts.data());
    }
    // Pass 2: Shadows & checkerboard overlays (tested against opaque depth + stencil)
    for (size_t bi = 0; bi < used_buckets; ++bi) {
        const PolyBucket &b = buckets[bi];
        if (b.depth_mode == Depth::Write || b.verts.empty()) continue;
        flush_run(b.prog, b.clip, b.blend, b.depth_mode, b.stencil, b.verts.size() / 4, b.verts.data());
    }
    g.clip = -1;
}

void GpuGlesRenderer::draw_frame(const model1::gpu_parts &parts, int w2d, AspectMode aspect,
                                 const gpu3d_poly *polys, size_t poly_count,
                                 bool arcade_shadows, int render_scale) {
    if (!ok_) return;
    const int s_scale = std::max(render_scale, 1);
    float sx = 1.0f, sy = 1.0f, ox = 0.0f, oy = 0.0f;
    compute_viewport(w2d, aspect, sx, sy, ox, oy);
    const float psx = sx / float(s_scale);
    const float psy = sy / float(s_scale);

    for (int t = 0; t < model1::TEX_COUNT; ++t) {
        if (t == model1::TEX_3D || !parts.img[t].px) continue;
        if (t == model1::TEX_SKY) update_tex_rgb565(layers_[t], parts.img[t], true);
        else update_tex_rgba5551(layers_[t], parts.img[t], false);
    }

    last_poly_count_ = 0;
    last_batch_count_ = 0;

    for (const model1::gpu_op &o : parts.ops) {
        const float dx0 = ox + o.dx * psx;
        const float dy0 = oy + o.dy * psy;
        const float dx1 = dx0 + o.dw * psx;
        const float dy1 = dy0 + o.dh * psy;
        const int cx0 = std::clamp(int(std::floor(ox + float(o.clip_x0) * psx)), 0, g.display_w);
        const int cx1 = (o.clip_x1 > 1000000) ? g.display_w : std::clamp(int(std::ceil(ox + float(o.clip_x1) * psx)), 0, g.display_w);
        if (cx1 <= cx0) continue;
        g.clip = add_clip({cx0, 0, cx1, g.display_h});

        if (o.tex == model1::TEX_3D) {
            g.clip = -1;
            emit_3d_polys(polys, poly_count, sx, sy, ox, oy, arcade_shadows, s_scale);
            continue;
        }
        if (o.tex < 0) {
            push_quad(Prog::Flat, 0, dx0, dy0, dx1, dy1, 0.0f, 0.0f, 0.0f, 0.0f,
                      xrgb_to_rgba(o.colour), false, StencilMode::Off);
            continue;
        }
        const model1::gpu_image &im = parts.img[o.tex];
        const LayerTex2D &lt = layers_[o.tex];
        const uint32_t tex = lt.active();
        if (!tex || im.w <= 0 || im.h <= 0) continue;
        const float inv_w = 1.0f / float(im.w);
        const float inv_h = 1.0f / float(im.h);
        const float u0 = o.sx * inv_w;
        const float v0 = o.sy * inv_h;
        const float u1 = (o.sx + o.sw) * inv_w;
        const float v1 = (o.sy + o.sh) * inv_h;
        const bool opaque = (o.tex == model1::TEX_SKY);
        push_quad(Prog::Ui, tex, dx0, dy0, dx1, dy1, u0, v0, u1, v1, 0xffffffffu, !opaque, StencilMode::Off);
    }
    g.clip = -1;
}

void GpuGlesRenderer::draw_cpu_framebuffer(const uint32_t *xrgb, int w, int h, AspectMode aspect) {
    if (!ok_ || !xrgb || w <= 0 || h <= 0) return;
    float sx = 1.0f, sy = 1.0f, ox = 0.0f, oy = 0.0f;
    compute_viewport(w, aspect, sx, sy, ox, oy);
    update_texture_rgba(tex_cpu_, cpu_w_, cpu_h_, w, h, false, xrgb, size_t(w));
    if (tex_cpu_) {
        push_quad(Prog::Ui, tex_cpu_, ox, oy, ox + float(w) * sx, oy + float(h) * sy,
                  0.0f, 0.0f, 1.0f, 1.0f, 0xffffffffu, false, StencilMode::Off);
    }
}

} // namespace arm64
