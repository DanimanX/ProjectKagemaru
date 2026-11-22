// Native OpenGL ES 2.0 / Desktop OpenGL 2.1 renderer for ProjectKagemaru
// (Anbernic RG34XX Mali-G31 MP2 and compatible AArch64 Linux handhelds).
//
// * Renders Sega Model 1's gpu_parts (System 24 2D tilemaps, stage sky panorama,
//   painted widescreen sides) + flat-shaded 3D polygons & wireframe lines directly
//   on Mali-G31 via GLES 2.0, with zero CPU 3D rasterization.
// * Uses the hardware 8-bit stencil buffer to enforce exact Model 1 smooth shadow
//   rules (blend once per pixel, fighter shadows excluded from fighter polygons).
// * ARM64 NEON 0x00RRGGBB / KEY(0xff000000) -> RGB565 / RGBA5551 dirty-row uploads.
#pragma once

#include "gpu/gpu3d.h"
#include "machine/model1.h"

#include <SDL.h>
#include <cstddef>
#include <cstdint>
#include <vector>

#ifndef RGBA8
#define RGBA8(r, g, b, a) \
    ((((uint32_t)(a) & 0xFFu) << 24) | (((uint32_t)(b) & 0xFFu) << 16) | \
     (((uint32_t)(g) & 0xFFu) << 8) | (((uint32_t)(r) & 0xFFu) << 0))
#endif

namespace arm64 {

enum class AspectMode : int {
    Widescreen32 = 0, // Native 3:2 widened Model 1 frustum (558x384 -> 720x480 on RG34XX)
    Original43 = 1,   // Authentic 4:3 (496x384 -> 620x480 centered)
    StretchFull = 2   // 496x384 stretched to full display
};

struct GlConfig {
    bool fullscreen = true;
    bool vsync = true;
    int window_w = 720;
    int window_h = 480;
};

bool gl_init(const GlConfig &cfg);
const char *gl_error();
const char *gl_renderer_string();
bool gl_is_gles();
int gl_stencil_bits();
SDL_Window *gl_window();
int gl_display_width();
int gl_display_height();
void gl_update_drawable_size();
void gl_set_vsync(bool enabled);
void gl_begin_frame();
void gl_end_frame();
void gl_fini();
void gl_fill_rect(float x, float y, float w, float h, uint32_t rgba);

struct GlUiVertex { float x, y, u, v; uint32_t rgba; };
uint32_t gl_create_ui_texture(int w, int h, const void *rgba);
void gl_delete_ui_texture(uint32_t texture);
void gl_ui_triangles(uint32_t texture, const GlUiVertex *vertices, size_t count, int x0, int y0, int x1, int y1);

// Lightweight gpu3d implementation that captures Model 1's projected 3D polygons
// into a flat vector for direct GLES 2.0 submission without CPU rasterization.
class GlesPolyCollector final : public gpu3d {
public:
    const char *name() const override { return "ProjectKagemaru GLES2"; }
    bool draw(const gpu3d_poly *, size_t, const gpu3d_span *, size_t, size_t,
              uint32_t *, size_t, int, int, int, int, double *) override {
        return false;
    }
    bool make_targets(int, int, int) override { return true; }
    bool draw_target(const gpu3d_poly *polys, size_t count, const gpu3d_span *, size_t, size_t,
                     int, int, int, int, int) override {
        if (polys && count) polys_.assign(polys, polys + count);
        else polys_.clear();
        return true;
    }
    const std::vector<gpu3d_poly> &polys() const { return polys_; }
    void clear() { polys_.clear(); }

private:
    std::vector<gpu3d_poly> polys_;
};

class GpuGlesRenderer {
public:
    GpuGlesRenderer();
    ~GpuGlesRenderer();
    GpuGlesRenderer(const GpuGlesRenderer &) = delete;
    GpuGlesRenderer &operator=(const GpuGlesRenderer &) = delete;

    bool ok() const { return ok_; }
    void shutdown();
    void reset_caches();

    void draw_frame(const model1::gpu_parts &parts, int w2d, AspectMode aspect,
                    const gpu3d_poly *polys, size_t poly_count,
                    bool arcade_shadows, int render_scale);
    void draw_cpu_framebuffer(const uint32_t *xrgb, int w, int h, AspectMode aspect);

    // Diagnostics
    size_t last_poly_count() const { return last_poly_count_; }
    size_t last_batch_count() const { return last_batch_count_; }

private:
    struct LayerTex2D {
        uint32_t tex[2] = {0, 0};
        uint32_t cur = 0;
        int w = 0, h = 0;
        int stride_scale = 1;
        bool linear = false;
        uint64_t last_gen = UINT64_MAX;
        std::vector<uint16_t> shadow[2];
        uint32_t active() const { return tex[cur]; }
    };

    uint32_t make_texture(uint32_t w, uint32_t h, bool linear, uint32_t wrap_s, uint32_t wrap_t,
                          uint32_t format, const void *pixels);
    void update_texture_rgba(uint32_t &tex, int &cur_w, int &cur_h, int w, int h, bool linear,
                             const uint32_t *xrgb, size_t stride_px);
    void update_tex_rgb565(LayerTex2D &layer, const model1::gpu_image &im, bool linear);
    void update_tex_rgba5551(LayerTex2D &layer, const model1::gpu_image &im, bool linear);
    void emit_3d_polys(const gpu3d_poly *polys, size_t count,
                       float sx, float sy, float ox, float oy,
                       bool arcade_shadows, int render_scale);

    bool ok_ = false;
    bool shutdown_ = false;

    LayerTex2D layers_[model1::TEX_COUNT];
    uint32_t tex_cpu_ = 0;
    int cpu_w_ = 0, cpu_h_ = 0;
    std::vector<uint32_t> swizzle_scratch_;
    size_t last_poly_count_ = 0;
    size_t last_batch_count_ = 0;
};

} // namespace arm64
