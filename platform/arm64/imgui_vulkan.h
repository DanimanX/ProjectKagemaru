// Dear ImGui SDL3 + SDL_GPU3 (Vulkan) backend wrapper for ProjectKagemaru.
#pragma once

#include "gpu_vulkan.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

#include <algorithm>

namespace arm64 {

void vk_set_imgui_active(bool active);

class ImGuiVulkan {
public:
    bool init() {
        SDL_Window *win = gl_window();
        SDL_GPUDevice *dev = vk_device();
        if (!win || !dev) return false;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;

        ImFontConfig font;
        font.SizePixels = gl_display_height() <= 480 ? 18.0f : 22.0f;
        io.Fonts->AddFontDefault(&font);

        ImGui::StyleColorsDark();
        auto &style = ImGui::GetStyle();
        style.WindowRounding = 6.0f;
        style.FrameRounding = 4.0f;
        style.FramePadding = {8.0f, 5.0f};
        style.ItemSpacing = {8.0f, 6.0f};
        style.ScrollbarSize = 18.0f;
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.08f, 0.13f, 0.94f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.22f, 0.68f, 0.90f, 0.45f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.08f, 0.45f, 0.72f, 1.0f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.10f, 0.52f, 0.80f, 0.85f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.68f, 0.96f, 0.95f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.12f, 0.44f, 0.68f, 0.85f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.65f, 0.92f, 0.95f);

        if (!ImGui_ImplSDL3_InitForSDLGPU(win)) {
            ImGui::DestroyContext();
            return false;
        }
        ImGui_ImplSDLGPU3_InitInfo info{};
        info.Device = dev;
        info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(dev, win);
        info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
        if (!ImGui_ImplSDLGPU3_Init(&info)) {
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
            return false;
        }
        initialized_ = true;
        vk_set_imgui_active(true);
        return true;
    }

    void frame(float dt) {
        if (!initialized_) return;
        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        auto &io = ImGui::GetIO();
        io.DisplaySize = {float(gl_display_width()), float(gl_display_height())};
        io.DeltaTime = std::clamp(dt, 0.001f, 0.1f);
        ImGui::NewFrame();
    }

    void render() {
        if (!initialized_) return;
        ImGui::Render();
    }

    void shutdown() {
        vk_set_imgui_active(false);
        if (initialized_) {
            if (SDL_GPUDevice *dev = vk_device()) {
                SDL_WaitForGPUIdle(dev);
            }
            ImGui_ImplSDLGPU3_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            initialized_ = false;
        }
        if (ImGui::GetCurrentContext()) {
            ImGui::DestroyContext();
        }
    }

private:
    bool initialized_ = false;
};

using ImGuiGles = ImGuiVulkan;

} // namespace arm64
