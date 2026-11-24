// Native SDL3 SDL_GPU (Vulkan + SPIR-V) renderer implementation for ProjectKagemaru.
#include "gpu_vulkan.h"
#include "gpu_vulkan_shaders.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace arm64 {

namespace {

enum class QuadBlend : uint8_t { Opaque, Keyed };

struct QuadDraw {
    SDL_GPUTexture *tex = nullptr;
    bool linear = false;
    QuadBlend blend = QuadBlend::Opaque;
    float dst_x = -1.0f, dst_y = -1.0f, dst_w = 2.0f, dst_h = 2.0f;
    float src_u = 0.0f, src_v = 0.0f, src_uw = 1.0f, src_vh = 1.0f;
    float solid_r = 0.0f, solid_g = 0.0f, solid_b = 0.0f, solid_a = 1.0f;
    bool force_opaque = false;
    bool use_solid = false;
    bool has_scissor = false;
    SDL_Rect scissor = {0, 0, 0, 0};
};

struct PendingUpload {
    SDL_GPUTexture *tex = nullptr;
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t offset = 0;
};

struct VkState {
    SDL_Window *window = nullptr;
    SDL_GPUDevice *dev = nullptr;
    SDL_GPUShader *vert_shader = nullptr;
    SDL_GPUShader *frag_shader = nullptr;
    SDL_GPUGraphicsPipeline *pipe_opaque = nullptr;
    SDL_GPUGraphicsPipeline *pipe_keyed = nullptr;
    SDL_GPUSampler *samp_nearest = nullptr;
    SDL_GPUSampler *samp_linear = nullptr;
    SDL_GPUTexture *white_tex = nullptr;
    SDL_GPUTransferBuffer *upload_buf = nullptr;
    uint32_t upload_cap = 0;
    uint32_t upload_used = 0;
    uint8_t *upload_mapped = nullptr;

    int disp_w = 1280;
    int disp_h = 720;
    bool vsync = true;
    bool imgui_active = false;

    std::string error;
    std::string renderer_str = "Vulkan (SDL_GPU)";

    std::vector<QuadDraw> quads;
    std::vector<PendingUpload> uploads;
} g;

void compute_viewport(int w2d, AspectMode aspect, float &vx, float &vy, float &vw, float &vh) {
    const float dw = float(std::max(1, g.disp_w));
    const float dh = float(std::max(1, g.disp_h));
    if (aspect == AspectMode::StretchFull) {
        vx = 0.0f; vy = 0.0f; vw = dw; vh = dh;
        return;
    }
    const float target_ar = (aspect == AspectMode::Original43)
        ? (496.0f / 384.0f)
        : (float(std::max(1, w2d)) / 384.0f);
    const float disp_ar = dw / dh;
    if (disp_ar > target_ar) {
        vh = dh;
        vw = dh * target_ar;
        vx = (dw - vw) * 0.5f;
        vy = 0.0f;
    } else {
        vw = dw;
        vh = dw / target_ar;
        vx = 0.0f;
        vy = (dh - vh) * 0.5f;
    }
}

void px_to_ndc(float px, float py, float pw, float ph, float &nx, float &ny, float &nw, float &nh) {
    const float dw = float(std::max(1, g.disp_w));
    const float dh = float(std::max(1, g.disp_h));
    nx = (px / dw) * 2.0f - 1.0f;
    ny = (py / dh) * 2.0f - 1.0f;
    nw = (pw / dw) * 2.0f;
    nh = (ph / dh) * 2.0f;
}

bool ensure_upload_capacity(uint32_t needed_bytes) {
    if (g.upload_buf && g.upload_cap >= needed_bytes) return true;
    if (g.upload_mapped) {
        SDL_UnmapGPUTransferBuffer(g.dev, g.upload_buf);
        g.upload_mapped = nullptr;
    }
    if (g.upload_buf) {
        SDL_ReleaseGPUTransferBuffer(g.dev, g.upload_buf);
        g.upload_buf = nullptr;
        g.upload_cap = 0;
    }
    const uint32_t cap = std::max(needed_bytes, 8u * 1024u * 1024u);
    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size = cap;
    g.upload_buf = SDL_CreateGPUTransferBuffer(g.dev, &tbi);
    if (!g.upload_buf) return false;
    g.upload_cap = cap;
    return true;
}

void queue_texture_upload(SDL_GPUTexture *tex, int w, int h, const uint32_t *pixels, size_t stride_px) {
    if (!tex || !pixels || w <= 0 || h <= 0) return;
    const uint32_t row_bytes = uint32_t(w) * 4u;
    const uint32_t total_bytes = row_bytes * uint32_t(h);
    const uint32_t aligned_off = (g.upload_used + 15u) & ~15u;
    if (!ensure_upload_capacity(aligned_off + total_bytes)) return;
    if (!g.upload_mapped) {
        g.upload_mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(g.dev, g.upload_buf, true));
        if (!g.upload_mapped) return;
    }
    uint8_t *dst = g.upload_mapped + aligned_off;
    for (int y = 0; y < h; ++y) {
        std::memcpy(dst + size_t(y) * row_bytes, pixels + size_t(y) * stride_px, row_bytes);
    }
    g.uploads.push_back({tex, uint32_t(w), uint32_t(h), aligned_off});
    g.upload_used = aligned_off + total_bytes;
}

SDL_GPUGraphicsPipeline *make_quad_pipeline(SDL_GPUTextureFormat fmt, QuadBlend blend) {
    SDL_GPUColorTargetBlendState bs{};
    if (blend == QuadBlend::Keyed) {
        // Model 1 keyed alpha: 0x00RRGGBB (alpha=0) is opaque, KEY 0xff000000 (alpha=1) is transparent,
        // and gpu3d_target shadows have alpha=128/255. out = src * (1 - a) + dst * a.
        bs.enable_blend = true;
        bs.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        bs.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        bs.color_blend_op = SDL_GPU_BLENDOP_ADD;
        bs.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        bs.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        bs.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }
    SDL_GPUColorTargetDescription ctd{};
    ctd.format = fmt;
    ctd.blend_state = bs;

    SDL_GPUGraphicsPipelineCreateInfo pci{};
    pci.vertex_shader = g.vert_shader;
    pci.fragment_shader = g.frag_shader;
    pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    pci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pci.target_info.num_color_targets = 1;
    pci.target_info.color_target_descriptions = &ctd;
    return SDL_CreateGPUGraphicsPipeline(g.dev, &pci);
}

void apply_present_mode() {
    if (!g.dev || !g.window) return;
    SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
    if (!g.vsync) {
        if (SDL_WindowSupportsGPUPresentMode(g.dev, g.window, SDL_GPU_PRESENTMODE_IMMEDIATE)) {
            mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
        } else if (SDL_WindowSupportsGPUPresentMode(g.dev, g.window, SDL_GPU_PRESENTMODE_MAILBOX)) {
            mode = SDL_GPU_PRESENTMODE_MAILBOX;
        }
    }
    SDL_SetGPUSwapchainParameters(g.dev, g.window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
}

} // namespace

void vk_set_imgui_active(bool active) { g.imgui_active = active; }

bool gl_init(const GlConfig &cfg) {
    g.error.clear();
    g.vsync = cfg.vsync;
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            g.error = std::string("SDL_InitSubSystem(VIDEO) failed: ") + SDL_GetError();
            return false;
        }
    }
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
    if (cfg.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;

    g.window = SDL_CreateWindow("ProjectKagemaru — Virtua Fighter (Vulkan)",
                                cfg.window_w > 0 ? cfg.window_w : 1280,
                                cfg.window_h > 0 ? cfg.window_h : 720, flags);
    if (!g.window) {
        g.error = std::string("SDL_CreateWindow(Vulkan) failed: ") + SDL_GetError();
        return false;
    }

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, std::getenv("GPU3D_VALIDATE") != nullptr);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_CLIP_DISTANCE_BOOLEAN, false);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_DEPTH_CLAMPING_BOOLEAN, false);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_INDIRECT_DRAW_FIRST_INSTANCE_BOOLEAN, false);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_ANISOTROPY_BOOLEAN, false);
    SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "vulkan");
    g.dev = SDL_CreateGPUDeviceWithProperties(props);
    SDL_DestroyProperties(props);
    if (!g.dev) {
        g.error = std::string("SDL_CreateGPUDevice(Vulkan) failed: ") + SDL_GetError();
        gl_fini();
        return false;
    }

    if (!SDL_ClaimWindowForGPUDevice(g.dev, g.window)) {
        g.error = std::string("SDL_ClaimWindowForGPUDevice failed: ") + SDL_GetError();
        gl_fini();
        return false;
    }

    apply_present_mode();
    gl_update_drawable_size();

    if (const char *drv = SDL_GetGPUDeviceDriver(g.dev)) {
        const char *dn = SDL_GetStringProperty(SDL_GetGPUDeviceProperties(g.dev), SDL_PROP_GPU_DEVICE_NAME_STRING, nullptr);
        g.renderer_str = std::string("SDL_GPU ") + drv + (dn ? (std::string(" (") + dn + ")") : std::string());
    }

    SDL_GPUShaderCreateInfo vsi{};
    vsi.code = kQuadVertSpv;
    vsi.code_size = sizeof(kQuadVertSpv);
    vsi.entrypoint = "main";
    vsi.format = SDL_GPU_SHADERFORMAT_SPIRV;
    vsi.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vsi.num_uniform_buffers = 1;
    g.vert_shader = SDL_CreateGPUShader(g.dev, &vsi);

    SDL_GPUShaderCreateInfo fsi{};
    fsi.code = kQuadFragSpv;
    fsi.code_size = sizeof(kQuadFragSpv);
    fsi.entrypoint = "main";
    fsi.format = SDL_GPU_SHADERFORMAT_SPIRV;
    fsi.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fsi.num_samplers = 1;
    fsi.num_uniform_buffers = 1;
    g.frag_shader = SDL_CreateGPUShader(g.dev, &fsi);

    const SDL_GPUTextureFormat sc_fmt = SDL_GetGPUSwapchainTextureFormat(g.dev, g.window);
    if (g.vert_shader && g.frag_shader) {
        g.pipe_opaque = make_quad_pipeline(sc_fmt, QuadBlend::Opaque);
        g.pipe_keyed = make_quad_pipeline(sc_fmt, QuadBlend::Keyed);
    }

    SDL_GPUSamplerCreateInfo sci{};
    sci.min_filter = SDL_GPU_FILTER_NEAREST;
    sci.mag_filter = SDL_GPU_FILTER_NEAREST;
    sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    g.samp_nearest = SDL_CreateGPUSampler(g.dev, &sci);

    sci.min_filter = SDL_GPU_FILTER_LINEAR;
    sci.mag_filter = SDL_GPU_FILTER_LINEAR;
    g.samp_linear = SDL_CreateGPUSampler(g.dev, &sci);

    SDL_GPUTextureCreateInfo wci{};
    wci.type = SDL_GPU_TEXTURETYPE_2D;
    wci.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    wci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    wci.width = 1;
    wci.height = 1;
    wci.layer_count_or_depth = 1;
    wci.num_levels = 1;
    wci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    g.white_tex = SDL_CreateGPUTexture(g.dev, &wci);
    if (g.white_tex) {
        const uint32_t white = 0xffffffffu;
        queue_texture_upload(g.white_tex, 1, 1, &white, 1);
    }

    if (!g.pipe_opaque || !g.pipe_keyed || !g.samp_nearest || !g.samp_linear || !g.white_tex) {
        g.error = std::string("SDL_GPU pipeline/sampler init failed: ") + SDL_GetError();
        gl_fini();
        return false;
    }

    return true;
}

const char *gl_error() { return g.error.c_str(); }
const char *gl_renderer_string() { return g.renderer_str.c_str(); }
SDL_Window *gl_window() { return g.window; }
SDL_GPUDevice *vk_device() { return g.dev; }
int gl_display_width() { return g.disp_w; }
int gl_display_height() { return g.disp_h; }

void gl_update_drawable_size() {
    if (!g.window) return;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g.window, &w, &h);
    if (w <= 0 || h <= 0) SDL_GetWindowSize(g.window, &w, &h);
    g.disp_w = std::max(1, w);
    g.disp_h = std::max(1, h);
}

void gl_set_vsync(bool enabled) {
    g.vsync = enabled;
    apply_present_mode();
}

void gl_process_event(const SDL_Event &e) {
    if (g.imgui_active) ImGui_ImplSDL3_ProcessEvent(&e);
}

void gl_begin_frame() {
    g.quads.clear();
    // Keep any initial white_tex upload queued during gl_init() if not flushed yet.
    if (g.uploads.empty()) g.upload_used = 0;
}

void gl_end_frame() {
    if (!g.dev || !g.window) return;

    SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(g.dev);
    if (!cb) return;

    if (g.upload_mapped) {
        SDL_UnmapGPUTransferBuffer(g.dev, g.upload_buf);
        g.upload_mapped = nullptr;
    }
    if (!g.uploads.empty() && g.upload_buf) {
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
        for (const auto &u : g.uploads) {
            SDL_GPUTextureTransferInfo src{};
            src.transfer_buffer = g.upload_buf;
            src.offset = u.offset;
            src.pixels_per_row = u.w;
            src.rows_per_layer = u.h;
            SDL_GPUTextureRegion dst{};
            dst.texture = u.tex;
            dst.w = u.w;
            dst.h = u.h;
            dst.d = 1;
            SDL_UploadToGPUTexture(cp, &src, &dst, false);
        }
        SDL_EndGPUCopyPass(cp);
        g.uploads.clear();
        g.upload_used = 0;
    }

    ImDrawData *draw_data = g.imgui_active ? ImGui::GetDrawData() : nullptr;
    if (draw_data) {
        ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, cb);
    }

    SDL_GPUTexture *swap_tex = nullptr;
    Uint32 sw = 0, sh = 0;
    if (SDL_WaitAndAcquireGPUSwapchainTexture(cb, g.window, &swap_tex, &sw, &sh) && swap_tex) {
        if (sw > 0 && sh > 0) {
            g.disp_w = int(sw);
            g.disp_h = int(sh);
        }
        SDL_GPUColorTargetInfo cti{};
        cti.texture = swap_tex;
        cti.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
        cti.load_op = SDL_GPU_LOADOP_CLEAR;
        cti.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cb, &cti, 1, nullptr);
        const SDL_Rect full_scissor{0, 0, g.disp_w, g.disp_h};
        for (const auto &q : g.quads) {
            if (!q.tex) continue;
            SDL_SetGPUScissor(rp, q.has_scissor ? &q.scissor : &full_scissor);
            SDL_GPUGraphicsPipeline *pipe = (q.blend == QuadBlend::Opaque) ? g.pipe_opaque : g.pipe_keyed;
            SDL_BindGPUGraphicsPipeline(rp, pipe);
            SDL_GPUTextureSamplerBinding tsb{q.tex, q.linear ? g.samp_linear : g.samp_nearest};
            SDL_BindGPUFragmentSamplers(rp, 0, &tsb, 1);
            const float v_ubo[8] = {q.dst_x, q.dst_y, q.dst_w, q.dst_h,
                                    q.src_u, q.src_v, q.src_uw, q.src_vh};
            const float f_ubo[8] = {q.solid_r, q.solid_g, q.solid_b, q.solid_a,
                                    q.force_opaque ? 1.0f : 0.0f,
                                    q.use_solid ? 1.0f : 0.0f,
                                    0.0f, 0.0f};
            SDL_PushGPUVertexUniformData(cb, 0, v_ubo, sizeof(v_ubo));
            SDL_PushGPUFragmentUniformData(cb, 0, f_ubo, sizeof(f_ubo));
            SDL_DrawGPUPrimitives(rp, 4, 1, 0, 0);
        }
        SDL_SetGPUScissor(rp, &full_scissor);
        if (draw_data) {
            ImGui_ImplSDLGPU3_RenderDrawData(draw_data, cb, rp);
        }
        SDL_EndGPURenderPass(rp);
    }

    SDL_SubmitGPUCommandBuffer(cb);
}

void gl_fini() {
    if (g.dev) {
        SDL_WaitForGPUIdle(g.dev);
        if (g.upload_mapped) {
            SDL_UnmapGPUTransferBuffer(g.dev, g.upload_buf);
            g.upload_mapped = nullptr;
        }
        if (g.upload_buf) { SDL_ReleaseGPUTransferBuffer(g.dev, g.upload_buf); g.upload_buf = nullptr; }
        if (g.white_tex) { SDL_ReleaseGPUTexture(g.dev, g.white_tex); g.white_tex = nullptr; }
        if (g.samp_nearest) { SDL_ReleaseGPUSampler(g.dev, g.samp_nearest); g.samp_nearest = nullptr; }
        if (g.samp_linear) { SDL_ReleaseGPUSampler(g.dev, g.samp_linear); g.samp_linear = nullptr; }
        if (g.pipe_opaque) { SDL_ReleaseGPUGraphicsPipeline(g.dev, g.pipe_opaque); g.pipe_opaque = nullptr; }
        if (g.pipe_keyed) { SDL_ReleaseGPUGraphicsPipeline(g.dev, g.pipe_keyed); g.pipe_keyed = nullptr; }
        if (g.vert_shader) { SDL_ReleaseGPUShader(g.dev, g.vert_shader); g.vert_shader = nullptr; }
        if (g.frag_shader) { SDL_ReleaseGPUShader(g.dev, g.frag_shader); g.frag_shader = nullptr; }
        if (g.window) SDL_ReleaseWindowFromGPUDevice(g.dev, g.window);
        SDL_DestroyGPUDevice(g.dev);
        g.dev = nullptr;
    }
    if (g.window) {
        SDL_DestroyWindow(g.window);
        g.window = nullptr;
    }
    g.quads.clear();
    g.uploads.clear();
    g.upload_cap = 0;
    g.upload_used = 0;
}

GpuVulkanRenderer::GpuVulkanRenderer() {
    if (!g.dev) return;
    gpu3d_ = gpu3d_create_sdlgpu_on(g.dev);
    if (!gpu3d_) {
        g.error = "gpu3d_create_sdlgpu_on failed on Vulkan device";
        return;
    }
    ok_ = true;
}

GpuVulkanRenderer::~GpuVulkanRenderer() {
    shutdown();
}

void GpuVulkanRenderer::release_layer(LayerTex &lt) {
    if (lt.tex && g.dev) {
        SDL_ReleaseGPUTexture(g.dev, lt.tex);
    }
    lt.tex = nullptr;
    lt.w = lt.h = 0;
    lt.last_gen = UINT64_MAX;
}

void GpuVulkanRenderer::ensure_layer(LayerTex &lt, int w, int h) {
    if (lt.tex && lt.w == w && lt.h == h) return;
    release_layer(lt);
    SDL_GPUTextureCreateInfo tci{};
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    tci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tci.width = uint32_t(w);
    tci.height = uint32_t(h);
    tci.layer_count_or_depth = 1;
    tci.num_levels = 1;
    tci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    lt.tex = SDL_CreateGPUTexture(g.dev, &tci);
    lt.w = w;
    lt.h = h;
    lt.last_gen = UINT64_MAX;
}

void GpuVulkanRenderer::upload_layer(LayerTex &lt, int w, int h, const uint32_t *pixels, size_t stride_px) {
    if (!pixels || w <= 0 || h <= 0) return;
    ensure_layer(lt, w, h);
    queue_texture_upload(lt.tex, w, h, pixels, stride_px);
}

void GpuVulkanRenderer::reset_caches() {
    for (auto &lt : layers_) lt.last_gen = UINT64_MAX;
    layer_cpu_.last_gen = UINT64_MAX;
}

void GpuVulkanRenderer::shutdown() {
    if (shutdown_) return;
    shutdown_ = true;
    ok_ = false;
    if (g.dev) SDL_WaitForGPUIdle(g.dev);
    gpu3d_.reset();
    for (auto &lt : layers_) release_layer(lt);
    release_layer(layer_cpu_);
}

void GpuVulkanRenderer::draw_frame(const model1::gpu_parts &parts, int w2d, AspectMode aspect,
                                   const gpu3d_poly * /*polys*/, size_t poly_count,
                                   bool /*arcade_shadows*/, int render_scale) {
    if (!ok_) return;
    const int s_scale = std::max(render_scale, 1);
    float vx = 0, vy = 0, vw = 1, vh = 1;
    compute_viewport(w2d, aspect, vx, vy, vw, vh);
    const float sx = vw / float(std::max(1, w2d));
    const float sy = vh / 384.0f;
    const float psx = sx / float(s_scale);
    const float psy = sy / float(s_scale);

    for (int t = 0; t < model1::TEX_COUNT; ++t) {
        if (t == model1::TEX_3D && parts.d3_target >= 0) continue;
        const model1::gpu_image &im = parts.img[t];
        if (!im.px || im.w <= 0 || im.h <= 0) continue;
        LayerTex &lt = layers_[t];
        if (!lt.tex || lt.w != im.w || lt.h != im.h || im.gen == 0 || lt.last_gen != im.gen) {
            upload_layer(lt, im.w, im.h, im.px, im.stride);
            lt.last_gen = im.gen;
        }
    }

    last_poly_count_ = poly_count;
    last_batch_count_ = parts.ops.size();

    for (const model1::gpu_op &o : parts.ops) {
        const float dx0 = vx + o.dx * psx;
        const float dy0 = vy + o.dy * psy;
        const float dw_px = o.dw * psx;
        const float dh_px = o.dh * psy;
        const int cx0 = std::clamp(int(std::floor(vx + float(o.clip_x0) * psx)), 0, g.disp_w);
        const int cx1 = (o.clip_x1 > 1000000) ? g.disp_w : std::clamp(int(std::ceil(vx + float(o.clip_x1) * psx)), 0, g.disp_w);
        if (cx1 <= cx0) continue;

        float nx = 0, ny = 0, nw = 0, nh = 0;
        px_to_ndc(dx0, dy0, dw_px, dh_px, nx, ny, nw, nh);

        QuadDraw q{};
        q.dst_x = nx; q.dst_y = ny; q.dst_w = nw; q.dst_h = nh;
        q.has_scissor = (cx0 > 0 || cx1 < g.disp_w);
        q.scissor = {cx0, 0, cx1 - cx0, g.disp_h};

        if (o.tex == model1::TEX_3D) {
            if (parts.d3_target >= 0 && gpu3d_) {
                auto *tex3d = static_cast<SDL_GPUTexture *>(gpu3d_->target_handle(parts.d3_target));
                if (!tex3d) continue;
                const float tw = float(w2d * s_scale);
                const float th = float(384 * s_scale);
                q.tex = tex3d;
                q.blend = QuadBlend::Keyed;
                q.src_u = o.sx / tw;
                q.src_v = o.sy / th;
                q.src_uw = o.sw / tw;
                q.src_vh = o.sh / th;
                g.quads.push_back(q);
            } else if (layers_[model1::TEX_3D].tex) {
                const model1::gpu_image &im = parts.img[model1::TEX_3D];
                q.tex = layers_[model1::TEX_3D].tex;
                q.blend = QuadBlend::Keyed;
                q.src_u = o.sx / float(im.w);
                q.src_v = o.sy / float(im.h);
                q.src_uw = o.sw / float(im.w);
                q.src_vh = o.sh / float(im.h);
                g.quads.push_back(q);
            }
            continue;
        }

        if (o.tex < 0) {
            q.tex = g.white_tex;
            q.blend = QuadBlend::Opaque;
            q.use_solid = true;
            q.solid_r = float((o.colour >> 16) & 0xffu) / 255.0f;
            q.solid_g = float((o.colour >> 8) & 0xffu) / 255.0f;
            q.solid_b = float(o.colour & 0xffu) / 255.0f;
            q.solid_a = 1.0f;
            g.quads.push_back(q);
            continue;
        }

        const model1::gpu_image &im = parts.img[o.tex];
        SDL_GPUTexture *tex = layers_[o.tex].tex;
        if (!tex || im.w <= 0 || im.h <= 0) continue;
        const bool opaque = (o.tex == model1::TEX_SKY);
        q.tex = tex;
        q.linear = opaque;
        q.blend = opaque ? QuadBlend::Opaque : QuadBlend::Keyed;
        q.force_opaque = opaque;
        q.src_u = o.sx / float(im.w);
        q.src_v = o.sy / float(im.h);
        q.src_uw = o.sw / float(im.w);
        q.src_vh = o.sh / float(im.h);
        g.quads.push_back(q);
    }
}

void GpuVulkanRenderer::draw_cpu_framebuffer(const uint32_t *xrgb, int w, int h, AspectMode aspect) {
    if (!ok_ || !xrgb || w <= 0 || h <= 0) return;
    last_poly_count_ = 0;
    last_batch_count_ = 1;
    upload_layer(layer_cpu_, w, h, xrgb, size_t(w));
    if (!layer_cpu_.tex) return;
    float vx = 0, vy = 0, vw = 1, vh = 1;
    compute_viewport(w, aspect, vx, vy, vw, vh);
    float nx = 0, ny = 0, nw = 2, nh = 2;
    px_to_ndc(vx, vy, vw, vh, nx, ny, nw, nh);
    QuadDraw q{};
    q.tex = layer_cpu_.tex;
    q.linear = false;
    q.blend = QuadBlend::Opaque;
    q.force_opaque = true;
    q.dst_x = nx; q.dst_y = ny; q.dst_w = nw; q.dst_h = nh;
    q.src_u = 0.0f; q.src_v = 0.0f; q.src_uw = 1.0f; q.src_vh = 1.0f;
    g.quads.push_back(q);
}

} // namespace arm64
