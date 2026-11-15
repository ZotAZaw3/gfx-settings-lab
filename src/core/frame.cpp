#include "core/frame.h"

#include <algorithm>

#include "core/module.h"

namespace gfx {

std::vector<ModuleFactory>& moduleFactories() {
    static std::vector<ModuleFactory> f;
    return f;
}

const char* stageName(Stage s) {
    static const char* names[] = {"FrameSetup", "Shadow", "DepthPrepass", "GBuffer", "Lighting", "Forward",
                                  "Translucent", "PostRender", "Upscale", "PostOutput", "ToneMap", "PostLDR",
                                  "OutputTransform", "FrameGen", "UI"};
    int i = int(s);
    return (i >= 0 && i < int(Stage::Count)) ? names[i] : "?";
}

Pass& PassList::add(Stage stage, std::string name, PassFn fn, float order, EnabledFn enabled) {
    Pass p;
    p.name = std::move(name);
    p.owner = currentOwner;
    p.stage = stage;
    p.order = order;
    p.enabled = std::move(enabled);
    p.execute = std::move(fn);
    passes_.push_back(std::move(p));
    return passes_.back();
}

void PassList::sort() {
    std::stable_sort(passes_.begin(), passes_.end(), [](const Pass& a, const Pass& b) {
        if (a.stage != b.stage) return int(a.stage) < int(b.stage);
        return a.order < b.order;
    });
}

// ---------------------------------------------------------------------------------------
static void applySize(rhi::TextureDesc& d, TargetScale ts, float scale, Extent render, Extent output) {
    if (ts == TargetScale::Fixed) return;
    Extent base = ts == TargetScale::Render ? render : output;
    d.width = std::max(1u, uint32_t(float(base.width) * scale + 0.5f));
    d.height = std::max(1u, uint32_t(float(base.height) * scale + 0.5f));
}

rhi::Texture* RenderTargets::create(const std::string& name, rhi::TextureDesc desc, TargetScale ts, float scale) {
    auto it = textures_.find(name);
    if (it != textures_.end()) {
        // Same request -> reuse; different format/flags -> recreate.
        auto& e = entries_[name];
        if (e.desc.format == desc.format && e.desc.usage == desc.usage && e.ts == ts && e.scale == scale &&
            e.desc.mipLevels == desc.mipLevels && e.desc.samples == desc.samples && e.desc.type == desc.type &&
            e.desc.depthOrLayers == desc.depthOrLayers && (ts != TargetScale::Fixed || (e.desc.width == desc.width && e.desc.height == desc.height)))
            return it->second.get();
    }
    if (desc.name.empty()) desc.name = name;
    entries_[name] = {desc, ts, scale};
    applySize(desc, ts, scale, render_, output_);
    textures_[name] = device_.createTexture(desc);
    return textures_[name].get();
}

rhi::Texture* RenderTargets::get(const std::string& name) const {
    auto it = textures_.find(name);
    return it == textures_.end() ? nullptr : it->second.get();
}

void RenderTargets::remove(const std::string& name) {
    textures_.erase(name);
    entries_.erase(name);
}

void RenderTargets::resize(Extent render, Extent output) {
    render_ = render; output_ = output;
    for (auto& [name, e] : entries_) {
        if (e.ts == TargetScale::Fixed) continue;
        rhi::TextureDesc d = e.desc;
        applySize(d, e.ts, e.scale, render, output);
        auto& cur = textures_[name];
        if (cur && cur->desc().width == d.width && cur->desc().height == d.height) continue;
        cur.reset();
        cur = device_.createTexture(d);
    }
}

// ---------------------------------------------------------------------------------------
void PostChain::beginFrame(rhi::Texture* sceneColor, rhi::Texture* renderA, rhi::Texture* outA, rhi::Texture* outB) {
    src_ = sceneColor;
    sceneColor_ = sceneColor;
    renderScratch_ = renderA;
    outA_ = outA; outB_ = outB;
    inUpscale_ = false; atOutputRes_ = false;
    pendingDst_ = nullptr;
}

rhi::Texture* PostChain::dst() {
    if (pendingDst_) return pendingDst_;
    if (!inUpscale_) {
        // render-res ping-pong between scene.color and the render scratch
        pendingDst_ = (src_ == renderScratch_) ? sceneColor() : renderScratch_;
    } else {
        pendingDst_ = (src_ == outA_) ? outB_ : outA_;
    }
    return pendingDst_;
}

void PostChain::swap() {
    if (!pendingDst_) return;
    src_ = pendingDst_;
    pendingDst_ = nullptr;
    if (inUpscale_) atOutputRes_ = true;
}

// ---------------------------------------------------------------------------------------
void FrameContext::fullscreen(rhi::Pipeline* pipeline, rhi::Texture* target, const char* name) {
    rhi::RenderPassDesc rp;
    rp.colorCount = 1;
    rp.colors[0].texture = target;
    rp.colors[0].load = rhi::LoadOp::DontCare;
    rp.name = name;
    cmd.beginRenderPass(rp);
    cmd.bindPipeline(pipeline);
    cmd.draw(3);
    cmd.endRenderPass();
}

}  // namespace gfx
