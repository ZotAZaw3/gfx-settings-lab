#include "core/gui.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#if defined(GFX_HAS_D3D11)
#include <d3d11.h>
#include <imgui_impl_dx11.h>
#endif

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "core/engine.h"

namespace gfx {

CoreGui::~CoreGui() {
    if (!initialized_) return;
#if defined(GFX_HAS_D3D11)
    if (dx11_) ImGui_ImplDX11_Shutdown(); else
#endif
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void CoreGui::init() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = "imgui.ini";
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6.0f; st.FrameRounding = 4.0f; st.GrabRounding = 4.0f;
    st.Colors[ImGuiCol_WindowBg].w = 0.92f;
#if defined(GFX_HAS_D3D11)
    if (engine_.device().caps().backend == rhi::Backend::D3D11) {
        dx11_ = true;
        ImGui_ImplGlfw_InitForOther(engine_.window(), true);
        ImGui_ImplDX11_Init(static_cast<ID3D11Device*>(engine_.device().nativeDevice()),
                            static_cast<ID3D11DeviceContext*>(engine_.device().nativeContext()));
        initialized_ = true;
        return;
    }
#endif
    ImGui_ImplGlfw_InitForOpenGL(engine_.window(), true);
    ImGui_ImplOpenGL3_Init("#version 450");
    initialized_ = true;
}

bool CoreGui::wantsKeyboard() const { return initialized_ && ImGui::GetIO().WantCaptureKeyboard; }
bool CoreGui::wantsMouse() const { return initialized_ && ImGui::GetIO().WantCaptureMouse; }

void CoreGui::openDoc(const std::string& doc) {
    docName_ = doc;
    std::filesystem::path p = std::filesystem::path(GFX_SOURCE_DIR) / "docs" / (doc + ".md");
    std::ifstream in(p);
    if (in) { std::stringstream ss; ss << in.rdbuf(); docText_ = ss.str(); }
    else docText_ = "docs/" + doc + ".md is not written yet.";
    docOpen_ = true;
}

void CoreGui::render(FrameContext& ctx, bool full) {
#if defined(GFX_HAS_D3D11)
    if (dx11_) ImGui_ImplDX11_NewFrame(); else
#endif
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    if (full) {
        if (showMenu) drawMenu(ctx);
        if (docOpen_) drawDoc();
        for (auto& m : engine_.modules()) m->onGui(ctx);
    }
    if (showOverlay && engine_.settings().getInt("debug.overlay") > 0) drawOverlay(ctx);
    ImGui::Render();
#if defined(GFX_HAS_D3D11)
    if (dx11_) { ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData()); return; }
#endif
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

// ---------------------------------------------------------------------------------------
static void costLabel(const SettingCost* c, bool measuring) {
    if (measuring) { ImGui::SameLine(); ImGui::TextDisabled("measuring..."); return; }
    if (!c || !c->delta.valid) return;
    ImGui::SameLine();
    const auto& d = c->delta;
    ImVec4 col = d.frameMs > 0.25 || d.gpuMs > 0.25 ? ImVec4(1.0f, 0.55f, 0.4f, 1) : (d.gpuMs < -0.25 ? ImVec4(0.5f, 1.0f, 0.5f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1));
    ImGui::TextColored(col, "GPU %+.2f  CPU %+.2f ms  VRAM %+.0f  RAM %+.0f MB", d.gpuMs, d.cpuMs, d.vramMB, d.ramMB);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Measured when you changed %s -> %s\n(%d frames after %d warm-up, vs %d frames before)",
                          c->from.c_str(), c->to.c_str(), c->framesMeasured, Profiler::kWarmup, Profiler::kWindow);
}

void CoreGui::drawMenu(FrameContext& ctx) {
    auto& reg = engine_.settings();
    auto& prof = engine_.profiler();
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(720, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Graphics Settings  (F1)", &showMenu)) { ImGui::End(); return; }

    // Preset row
    const char* presetNames[] = {"Low", "Medium", "High", "Ultra", "Custom"};
    int preset = int(reg.currentPreset());
    ImGui::SetNextItemWidth(160);
    if (ImGui::Combo("Quality Preset", &preset, presetNames, 5) && preset < 4) {
        engine_.applyingPreset_ = true;
        reg.applyPreset(Preset(preset));
        engine_.applyingPreset_ = false;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Show debug settings", &showDebugSettings);
    ImGui::SameLine();
    if (ImGui::Button("Save")) reg.save(engine_.config().settingsPath);
    ImGui::TextDisabled("Hover a setting for its definition, press ? for the full doc. Cost = measured delta after you change it.");
    ImGui::Separator();

    if (ImGui::BeginTabBar("categories", ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (const auto& cat : reg.categories()) {
            if (!ImGui::BeginTabItem(cat.c_str())) continue;
            ImGui::BeginChild("settings", ImVec2(0, 0), false);
            std::string group = "\x01";
            for (auto& s : reg.all()) {
                if (s.desc.category != cat) continue;
                if ((s.desc.flags & SettingDebug) && !showDebugSettings) continue;
                if (s.visibleIf && !s.visibleIf()) continue;
                if (s.desc.group != group) {
                    group = s.desc.group;
                    if (!group.empty()) ImGui::SeparatorText(group.c_str());
                }
                ImGui::PushID(s.desc.id.c_str());
                bool enabled = !s.enabledIf || s.enabledIf();
                ImGui::BeginDisabled(!enabled);
                float before = s.asNumber();
                bool changed = false;
                ImGui::SetNextItemWidth(260);
                switch (s.type) {
                    case SettingType::Bool: changed = ImGui::Checkbox(s.desc.label.c_str(), &s.b); break;
                    case SettingType::Int: changed = ImGui::SliderInt(s.desc.label.c_str(), &s.i, s.imin, s.imax); break;
                    case SettingType::Float: changed = ImGui::SliderFloat(s.desc.label.c_str(), &s.f, s.fmin, s.fmax, s.format); break;
                    case SettingType::Enum: {
                        if (ImGui::BeginCombo(s.desc.label.c_str(), s.valueString().c_str())) {
                            for (int i = 0; i < int(s.options.size()); ++i)
                                if (ImGui::Selectable(s.options[i].c_str(), s.i == i)) { s.i = i; changed = true; }
                            ImGui::EndCombo();
                        }
                        break;
                    }
                }
                // Sliders fire every drag step; commit (callbacks + profiler) on release to keep probes meaningful.
                bool commit = changed && (s.type == SettingType::Bool || s.type == SettingType::Enum);
                if (s.type == SettingType::Int || s.type == SettingType::Float) {
                    if (changed && !dragging_.count(s.desc.id)) dragging_[s.desc.id] = before;
                    if (ImGui::IsItemDeactivatedAfterEdit() && dragging_.count(s.desc.id)) {
                        before = dragging_[s.desc.id];
                        dragging_.erase(s.desc.id);
                        commit = s.asNumber() != before;
                    }
                }
                if (commit) reg.notifyChanged(s, before);
                ImGui::EndDisabled();
                if (!s.desc.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", s.desc.tooltip.c_str());
                if (s.desc.flags & SettingRequiresRestart) { ImGui::SameLine(); ImGui::TextDisabled("(restart)"); }
                if (!s.desc.doc.empty()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("?")) openDoc(s.desc.doc);
                }
                costLabel(prof.costOf(s.desc.id), prof.measuring() && prof.measuringId() == s.desc.id);
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void CoreGui::drawDoc() {
    ImGui::SetNextWindowPos(ImVec2(760, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(620, 720), ImGuiCond_FirstUseEver);
    std::string title = "Doc: " + docName_ + "###doc";
    if (!ImGui::Begin(title.c_str(), &docOpen_)) { ImGui::End(); return; }
    std::istringstream in(docText_);
    std::string line;
    bool code = false;
    while (std::getline(in, line)) {
        if (line.rfind("```", 0) == 0) { code = !code; continue; }
        if (code) { ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1), "%s", line.c_str()); continue; }
        if (line.rfind("# ", 0) == 0) { ImGui::SetWindowFontScale(1.3f); ImGui::TextUnformatted(line.c_str() + 2); ImGui::SetWindowFontScale(1.0f); continue; }
        if (line.rfind("## ", 0) == 0) { ImGui::SeparatorText(line.c_str() + 3); continue; }
        if (line.rfind("### ", 0) == 0) { ImGui::TextColored(ImVec4(1, 0.85f, 0.5f, 1), "%s", line.c_str() + 4); continue; }
        if (line.empty()) { ImGui::Spacing(); continue; }
        ImGui::TextWrapped("%s", line.c_str());
    }
    ImGui::End();
}

void CoreGui::drawOverlay(FrameContext& ctx) {
    auto& prof = engine_.profiler();
    int level = engine_.settings().getInt("debug.overlay");
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 12, vp->WorkPos.y + 12), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (level == 2) flags &= ~ImGuiWindowFlags_NoDecoration, flags |= ImGuiWindowFlags_NoTitleBar;
    if (!ImGui::Begin("Overlay", nullptr, flags)) { ImGui::End(); return; }
    FrameSample a = prof.average(30);
    double fps = a.frameMs > 0 ? 1000.0 / a.frameMs : 0.0;
    ImGui::Text("%.0f FPS  %.2f ms  (1%% low %.0f FPS)", fps, a.frameMs, 1000.0 / std::max(0.001, prof.percentileFrameMs(99, 300)));
    if (level >= 2) {
        auto& caps = engine_.device().caps();
        ImGui::TextDisabled("%s | %s", caps.apiName.c_str(), caps.renderer.c_str());
        ImGui::Text("CPU %.2f ms  GPU %.2f ms  wait %.2f ms", a.cpuMs, a.gpuMs, a.waitMs);
        ImGui::Text("CPU usage %.0f%%  GPU busy %.0f%%", prof.processCpuPercent(), prof.gpuBusyPercent());
        auto mem = prof.memory();
        ImGui::Text("RAM %.0f MB   VRAM (tracked) %.0f MB", a.ramMB, a.vramMB);
        if (mem.driverBudgetBytes) ImGui::Text("VRAM driver %.0f / %.0f MB", mem.driverUsedBytes / 1048576.0, mem.driverBudgetBytes / 1048576.0);
        ImGui::Text("Render %ux%u -> Output %ux%u (%.0f%%)", ctx.renderSize.width, ctx.renderSize.height, ctx.outputSize.width, ctx.outputSize.height,
                    engine_.renderScale * 100.0f);
        // Frame time graph
        static float graph[240];
        const auto& h = prof.history();
        int n = std::min<int>(240, int(h.size()));
        for (int i = 0; i < n; ++i) graph[i] = float(h[h.size() - n + i].frameMs);
        ImGui::PlotLines("##ft", graph, n, 0, "frame time (ms)", 0.0f, 40.0f, ImVec2(330, 60));

        if (ImGui::CollapsingHeader("Passes (GPU / CPU ms)", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::BeginTable("passes", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Stage"); ImGui::TableSetupColumn("Pass"); ImGui::TableSetupColumn("GPU"); ImGui::TableSetupColumn("CPU");
                ImGui::TableHeadersRow();
                for (auto& p : engine_.passList().passes()) {
                    if (!p.ranThisFrame) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextDisabled("%s", stageName(p.stage));
                    ImGui::TableNextColumn(); ImGui::Text("%s", p.name.c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("module: %s", p.owner.c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%.3f", p.gpuMs);
                    ImGui::TableNextColumn(); ImGui::Text("%.3f", p.cpuMs);
                }
                ImGui::EndTable();
            }
        }
        if (ImGui::CollapsingHeader("Memory by module")) {
            std::sort(mem.perOwner.begin(), mem.perOwner.end(), [](auto& x, auto& y) { return x.second > y.second; });
            for (auto& [owner, bytes] : mem.perOwner) ImGui::Text("%-28s %8.1f MB VRAM", owner.c_str(), bytes / 1048576.0);
            ImGui::Text("%-28s %8.1f MB RAM", "scene (CPU copies)", engine_.scene().cpuBytes() / 1048576.0);
            for (auto& m : engine_.modules())
                if (size_t b = m->cpuMemoryBytes()) ImGui::Text("%-28s %8.1f MB RAM", m->name(), b / 1048576.0);
        }
        if (ImGui::CollapsingHeader("Counters", ImGuiTreeNodeFlags_DefaultOpen))
            for (auto& [k, v] : prof.counters()) ImGui::Text("%-28s %.3f", k.c_str(), v);
        if (!engine_.shaders().lastError().empty()) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Shader error:");
            ImGui::TextWrapped("%s", engine_.shaders().lastError().c_str());
        }
    }
    ImGui::End();
}

}  // namespace gfx
