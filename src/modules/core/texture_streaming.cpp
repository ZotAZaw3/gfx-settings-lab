// Texture quality + texture streaming (docs/texture_quality.md, docs/texture_streaming.md).
//
// Every scene texture keeps its full RGBA8 mip chain in RAM (the "disk/streaming source"). The GPU
// only holds the mips that are needed: each frame we estimate the mip each texture needs on screen,
// cap it by Texture Quality, fit the result into the Streaming Pool budget (dropping detail from the
// least important textures first), then upload changes limited by the per-frame Upload Buffer size.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

#include "core/engine.h"

namespace gfx {

class TextureStreamingModule final : public Module {
public:
    const char* name() const override { return "core.textures"; }
    int priority() const override { return -50; }

    void registerSettings(SettingsRegistry& s) override {
        quality_ = &s.addEnum({.id = "textures.quality", .label = "Texture Quality", .category = "Textures", .group = "Quality",
                               .tooltip = "Maximum texture resolution kept on the GPU. Low 512, Medium 1024, High 2048, Ultra full resolution.",
                               .doc = "texture_quality"}, {"Low", "Medium", "High", "Ultra"}, 2)
                       .presets(0, 1, 2, 3);
        streaming_ = &s.addBool({.id = "textures.streaming", .label = "Texture Streaming", .category = "Textures", .group = "Streaming",
                                 .tooltip = "Load only the mip levels visible on screen, within the pool budget. Off = every texture fully resident.",
                                 .doc = "texture_streaming"}, true);
        pool_ = &s.addFloat({.id = "textures.poolSizeMB", .label = "Streaming Pool Size", .category = "Textures", .group = "Streaming",
                             .tooltip = "VRAM budget for streamed textures. When the visible set needs more, distant textures drop to lower mips.",
                             .doc = "texture_streaming"}, 512.0f, 16.0f, 4096.0f)
                    .fmt("%.0f MB").presets(128, 256, 512, 1024)
                    .visible([this] { return streaming_->asBool(); });
        upload_ = &s.addFloat({.id = "textures.uploadBufferMB", .label = "Streaming Buffer Size", .category = "Textures", .group = "Streaming",
                               .tooltip = "Buffered streaming size: how many MB of texture data may be uploaded per frame. Small = fewer hitches, slower pop-in.",
                               .doc = "texture_streaming"}, 16.0f, 0.5f, 128.0f)
                      .fmt("%.1f MB/frame").presets(4, 8, 16, 32)
                      .visible([this] { return streaming_->asBool(); });
        bias_ = &s.addInt({.id = "textures.streamingMipBias", .label = "Streaming Mip Bias", .category = "Textures", .group = "Streaming",
                           .tooltip = "Request N mips lower than needed (saves VRAM, blurrier).",
                           .doc = "texture_streaming"}, 0, 0, 3)
                    .visible([this] { return streaming_->asBool(); });
        showDebug_ = &s.addBool({.id = "textures.debugTint", .label = "Visualize Resident Mips", .category = "Textures", .group = "Streaming",
                                 .tooltip = "Overlay window listing each texture's resident vs wanted mip.", .doc = "texture_streaming",
                                 .flags = SettingDebug | SettingNoPreset}, false);
    }

    void update(FrameContext& ctx) override {
        Scene& scene = ctx.scene;
        if (scene.textures.empty()) return;
        const int n = int(scene.textures.size());

        // ---- 1. Quality cap: highest mip index allowed at the top ----
        static const uint32_t caps[] = {512, 1024, 2048, 1u << 30};
        uint32_t maxRes = caps[std::clamp(quality_->asInt(), 0, 3)];
        std::vector<int> wanted(n, 0), qualityTop(n, 0);
        for (int t = 0; t < n; ++t) {
            const auto& tex = scene.textures[t];
            int top = 0;
            while (std::max(tex.width >> top, tex.height >> top) > maxRes && top + 1 < int(tex.mipCount)) ++top;
            qualityTop[t] = top;
            wanted[t] = top;
        }

        // ---- 2. Visibility-driven wanted mip (screen-space texel density estimate) ----
        std::vector<float> importance(n, 0.0f);
        if (streaming_->asBool()) {
            std::vector<int> need(n, 64);   // 64 = not visible
            float screenH = float(ctx.renderSize.height);
            float tanHalf = std::tan(ctx.camera.fovY * 0.5f);
            for (auto& it : ctx.draws.opaque) estimate(ctx, it, need, importance, screenH, tanHalf);
            for (auto& it : ctx.draws.transparent) estimate(ctx, it, need, importance, screenH, tanHalf);
            for (int t = 0; t < n; ++t) {
                const auto& tex = scene.textures[t];
                int lowest = int(tex.mipCount) - 1;
                int w = need[t] == 64 ? std::min(lowest, std::max(0, lowest - 4)) : need[t];   // invisible: keep a 16px mip
                wanted[t] = std::clamp(std::max(w + bias_->asInt(), qualityTop[t]), 0, lowest);
            }
            // ---- 3. Fit into the pool: drop detail from the least important textures first ----
            size_t budget = size_t(pool_->asFloat() * 1024.0f * 1024.0f);
            auto total = [&] { size_t b = 0; for (int t = 0; t < n; ++t) b += chainBytes(scene.textures[t], wanted[t]); return b; };
            size_t bytes = total();
            std::vector<int> order(n);
            std::iota(order.begin(), order.end(), 0);
            std::sort(order.begin(), order.end(), [&](int a, int b) { return importance[a] < importance[b]; });
            overBudget_ = false;
            for (int pass = 0; pass < 16 && bytes > budget; ++pass) {
                bool changed = false;
                for (int t : order) {
                    if (bytes <= budget) break;
                    const auto& tex = scene.textures[t];
                    if (wanted[t] + 1 >= int(tex.mipCount) - 2) continue;   // keep at least a few small mips
                    size_t before = chainBytes(tex, wanted[t]);
                    ++wanted[t];
                    bytes -= before - chainBytes(tex, wanted[t]);
                    changed = true;
                }
                if (!changed) break;
            }
            overBudget_ = bytes > budget;
            wantedBytes_ = bytes;
        }

        // ---- 4. Upload changes within the per-frame buffer budget ----
        double budgetBytes = streaming_->asBool() ? double(upload_->asFloat()) * 1024.0 * 1024.0 : 1e18;
        double uploaded = 0.0;
        int pending = 0;
        // Upgrades of the most important textures first; downgrades are free (no upload of new data beyond a smaller chain).
        std::vector<int> order(n);
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return importance[a] > importance[b]; });
        for (int t : order) {
            auto& tex = scene.textures[t];
            if (int(tex.residentTopMip) == wanted[t] && tex.gpu) continue;
            size_t cost = chainBytes(tex, wanted[t]);
            bool downgrade = tex.gpu && wanted[t] > int(tex.residentTopMip);
            if (!downgrade && uploaded > 0.0 && uploaded + double(cost) > budgetBytes) { ++pending; continue; }
            makeResident(ctx.device, tex, uint32_t(wanted[t]));
            uploaded += double(cost);
        }
        uploadedMB_ = uploaded / (1024.0 * 1024.0);
        pending_ = pending;

        size_t resident = 0;
        for (auto& tex : scene.textures) resident += tex.gpu ? tex.gpu->gpuBytes() : 0;
        ctx.profiler.counter("textures.residentMB", double(resident) / (1024.0 * 1024.0));
        ctx.profiler.counter("textures.wantedMB", double(wantedBytes_) / (1024.0 * 1024.0));
        ctx.profiler.counter("textures.uploadedMB/frame", uploadedMB_);
        ctx.profiler.counter("textures.pendingUploads", pending_);
        ctx.profiler.counter("textures.overBudget", overBudget_ ? 1.0 : 0.0);
        wantedCache_ = wanted;
    }

    void onGui(FrameContext& ctx) override {
        if (!showDebug_->asBool()) return;
        auto& scene = ctx.scene;
        if (!ImGuiBegin()) return;
        for (size_t t = 0; t < scene.textures.size(); ++t) {
            auto& tex = scene.textures[t];
            int want = t < wantedCache_.size() ? wantedCache_[t] : 0;
            ImGuiLine(tex.name, tex.width, tex.height, tex.residentTopMip, want, tex.gpu ? tex.gpu->gpuBytes() : 0);
        }
        ImGuiEnd();
    }

    size_t cpuMemoryBytes() const override { return 0; }   // CPU mips are counted under "scene (CPU copies)"

private:
    static size_t chainBytes(const SceneTexture& t, int top) {
        size_t b = 0;
        for (uint32_t m = uint32_t(std::max(0, top)); m < t.mipCount; ++m) b += size_t(std::max(1u, t.width >> m)) * std::max(1u, t.height >> m) * 4;
        return b;
    }

    // Projected-size estimate: an object covering P pixels on screen needs about P texels across
    // its UV range; mip = log2(textureSize / (P / uvRepeat)).
    void estimate(FrameContext& ctx, const DrawItem& it, std::vector<int>& need, std::vector<float>& importance, float screenH, float tanHalf) {
        Scene& scene = ctx.scene;
        const Instance& inst = scene.instances[it.instance];
        const Primitive& prim = scene.primitives[it.primitive];
        const Material& m = scene.materials[prim.material];
        glm::vec3 c = inst.worldBounds.center();
        float radius = glm::length(inst.worldBounds.extent());
        float dist = std::max(0.1f, glm::length(c - ctx.camera.position) - radius);
        float projected = radius / (dist * tanHalf) * screenH;   // pixels across the object
        float uvRepeat = uvRepeatOf(scene, it.primitive);
        for (int idx : {m.baseColorTex, m.normalTex, m.metalRoughTex, m.occlusionTex, m.emissiveTex, m.heightTex}) {
            if (idx < 0) continue;
            const auto& tex = scene.textures[idx];
            float texelsNeeded = std::max(1.0f, projected / std::max(1e-3f, uvRepeat));
            int mip = int(std::floor(std::log2(std::max(1.0f, float(tex.width) / texelsNeeded))));
            need[idx] = std::min(need[idx], std::max(0, mip));
            importance[idx] = std::max(importance[idx], projected);
        }
    }

    float uvRepeatOf(Scene& scene, uint32_t prim) {
        if (prim < uvRepeat_.size() && uvRepeat_[prim] > 0.0f && geomVersion_ == scene.geometryVersion()) return uvRepeat_[prim];
        if (geomVersion_ != scene.geometryVersion()) { uvRepeat_.assign(scene.primitives.size(), 0.0f); geomVersion_ = scene.geometryVersion(); }
        const Primitive& p = scene.primitives[prim];
        glm::vec2 lo(1e9f), hi(-1e9f);
        for (uint32_t v = 0; v < p.vertexCount; v += std::max(1u, p.vertexCount / 256)) {
            lo = glm::min(lo, scene.vertices[p.baseVertex + v].uv);
            hi = glm::max(hi, scene.vertices[p.baseVertex + v].uv);
        }
        float r = std::max(1.0f, std::max(hi.x - lo.x, hi.y - lo.y));
        if (prim < uvRepeat_.size()) uvRepeat_[prim] = r;
        return r;
    }

    void makeResident(rhi::Device& dev, SceneTexture& tex, uint32_t top) {
        rhi::TextureDesc d;
        d.format = tex.srgb ? rhi::Format::RGBA8_SRGB : rhi::Format::RGBA8;
        d.width = std::max(1u, tex.width >> top);
        d.height = std::max(1u, tex.height >> top);
        d.mipLevels = tex.mipCount - top;
        d.name = tex.name;
        d.owner = "textures";
        auto gpu = dev.createTexture(d);
        for (uint32_t m = top; m < tex.mipCount; ++m) dev.uploadTexture(gpu.get(), m - top, 0, tex.cpuMips[m].data(), tex.cpuMips[m].size());
        tex.gpu = std::move(gpu);
        tex.residentTopMip = top;
    }

    // Tiny ImGui shim so this file doesn't depend on ImGui headers directly.
    bool ImGuiBegin();
    void ImGuiLine(const std::string& name, uint32_t w, uint32_t h, uint32_t resident, int wanted, size_t bytes);
    void ImGuiEnd();

    Setting *quality_ = nullptr, *streaming_ = nullptr, *pool_ = nullptr, *upload_ = nullptr, *bias_ = nullptr, *showDebug_ = nullptr;
    std::vector<float> uvRepeat_;
    uint64_t geomVersion_ = 0;
    std::vector<int> wantedCache_;
    size_t wantedBytes_ = 0;
    double uploadedMB_ = 0.0;
    int pending_ = 0;
    bool overBudget_ = false;
};

}  // namespace gfx

#include <imgui.h>
namespace gfx {
bool TextureStreamingModule::ImGuiBegin() {
    ImGui::SetNextWindowSize(ImVec2(520, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Texture streaming")) { ImGui::End(); return false; }
    ImGui::Text("%-24s %10s %8s %8s %9s", "texture", "size", "resident", "wanted", "VRAM");
    return true;
}
void TextureStreamingModule::ImGuiLine(const std::string& name, uint32_t w, uint32_t h, uint32_t resident, int wanted, size_t bytes) {
    ImVec4 col = int(resident) == wanted ? ImVec4(0.7f, 1, 0.7f, 1) : ImVec4(1, 0.8f, 0.4f, 1);
    ImGui::TextColored(col, "%-24.24s %4ux%-5u %8u %8d %7.1fMB", name.c_str(), w, h, resident, wanted, bytes / 1048576.0);
}
void TextureStreamingModule::ImGuiEnd() { ImGui::End(); }

GFX_REGISTER_MODULE(TextureStreamingModule);
}  // namespace gfx
