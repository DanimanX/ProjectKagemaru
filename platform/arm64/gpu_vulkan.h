// Native SDL3 SDL_GPU (Vulkan + SPIR-V) renderer for ProjectKagemaru
// (ARM64 Linux / Mesa Turnip / Adreno / Mali / Desktop Vulkan).
//
// Exposes the same arm64::GlConfig / gl_* / GpuGlesRenderer / ImGuiGles
// surface as gpu_gles.h so main_arm64.cpp drives both backends identically,
// while delegating 3D polygon rasterization to src/gpu/gpu3d_sdlgpu.cpp
// (pixel-exact Model 1 compute shaders in SPIR-V via draw_target).
#pragma once

#include "gpu/gpu3d.h"
#include "machine/model1.h"

#include <SDL3/SDL.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace arm64 {

enum class AspectMode : int {
    Widescreen32 = 0, // Native 3:2 widened Model 1 frustum (558x384 -> 720x480)
    Original43 = 1,   // Authentic 4:3 (496x384)
    StretchFull = 2   // Stretched to full display
};

struct GlConfig {
    bool fullscreen = true;
    bool vsync = true;
    int window_w = 1280;
    int window_h = 720;
};

bool gl_init(const GlConfig &cfg);
const char *gl_error();
const char *gl_renderer_string();
inline bool gl_is_gles() { return false; }
inline int gl_stencil_bits() { return 0; }
SDL_Window *gl_window();
SDL_GPUDevice *vk_device();
int gl_display_width();
int gl_display_height();
void gl_update_drawable_size();
void gl_set_vsync(bool enabled);
void gl_begin_frame();
void gl_end_frame();
void gl_fini();
void gl_process_event(const SDL_Event &e);

// Lightweight gpu3d implementation used only for headless --bench-* CLI modes
// when no Vulkan window/device is created.
class GlesPolyCollector final : public gpu3d {
public:
    const char *name() const override { return "ProjectKagemaru Headless Collector"; }
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

class GpuVulkanRenderer {
public:
    GpuVulkanRenderer();
    ~GpuVulkanRenderer();
    GpuVulkanRenderer(const GpuVulkanRenderer &) = delete;
    GpuVulkanRenderer &operator=(const GpuVulkanRenderer &) = delete;

    bool ok() const { return ok_; }
    void shutdown();
    void reset_caches();

    gpu3d *gpu3d_ptr() const { return gpu3d_.get(); }

    void draw_frame(const model1::gpu_parts &parts, int w2d, AspectMode aspect,
                    const gpu3d_poly *polys, size_t poly_count,
                    bool arcade_shadows, int render_scale);
    void draw_cpu_framebuffer(const uint32_t *xrgb, int w, int h, AspectMode aspect);

    // Diagnostics
    size_t last_poly_count() const { return last_poly_count_; }
    size_t last_batch_count() const { return last_batch_count_; }

private:
    struct LayerTex {
        SDL_GPUTexture *tex = nullptr;
        int w = 0;
        int h = 0;
        uint64_t last_gen = UINT64_MAX;
    };

    void ensure_layer(LayerTex &lt, int w, int h);
    void upload_layer(LayerTex &lt, int w, int h, const uint32_t *pixels, size_t stride_px);
    void release_layer(LayerTex &lt);

    bool ok_ = false;
    bool shutdown_ = false;

    std::unique_ptr<gpu3d> gpu3d_;
    LayerTex layers_[model1::TEX_COUNT];
    LayerTex layer_cpu_;

    size_t last_poly_count_ = 0;
    size_t last_batch_count_ = 0;
};

using GpuGlesRenderer = GpuVulkanRenderer;

} // namespace arm64
