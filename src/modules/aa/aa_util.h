// Shared helpers for the anti-aliasing / upscaling / frame generation modules.
#pragma once

#include <cmath>
#include <cstdint>
#include <initializer_list>

#include "core/engine.h"

namespace gfx::aa {

// Setting ids other modules may read.
inline constexpr const char* kMethod      = "aa.method";             // Off, FXAA, SMAA, TAA
inline constexpr const char* kMsaa        = "aa.msaa.samples";       // Off, 2x, 4x, 8x  -> samples = 1 << value
inline constexpr const char* kUpscaler    = "aa.upscaler";           // see Upscaler enum
inline constexpr const char* kQuality     = "aa.upscale.quality";
inline constexpr const char* kRayRecon    = "aa.ray_reconstruction"; // 0 Off, 1 RR-lite, 2 DLSS RR
inline constexpr const char* kSharpen     = "aa.sharpen.mode";
inline constexpr const char* kDebugView   = "aa.debug.view";
inline constexpr const char* kFrameGen    = "aa.framegen";
inline constexpr const char* kCategory    = "Anti-Aliasing & Upscaling";

enum Method   { MethodOff = 0, MethodFXAA, MethodSMAA, MethodTAA };
enum Upscaler { UpOff = 0, UpFSR1, UpNIS, UpTemporal, UpDLSS, UpFSR3, UpXeSS };
enum DebugView { DbgOff = 0, DbgMotion, DbgHistory, DbgRejection, DbgConfidence, DbgRrHistory, DbgFgSource };

// Per-axis render scale of each quality mode (same table DLSS / FSR / XeSS use).
inline constexpr float kQualityScale[] = {1.0f, 0.77f, 0.667f, 0.58f, 0.5f, 0.333f};

// Core jitter convention: pixel space, +x right, +y DOWN (same as uv). A positive jitter shifts the
// rasterized image by +jitter pixels in uv space (core applies translate(2jx/w, -2jy/h) to clip space).
inline constexpr float kJitterUvYSign = 1.0f;

inline float halton(uint32_t index, uint32_t base) {
    float f = 1.0f, r = 0.0f;
    while (index > 0) { f /= float(base); r += f * float(index % base); index /= base; }
    return r;
}

// Number of jitter phases so every output pixel gets covered: 8 * (output/render)^2, 8..64.
inline uint32_t jitterPhases(Extent render, Extent output) {
    float ratio = float(output.width) / float(render.width ? render.width : 1);
    float n = std::ceil(8.0f * ratio * ratio);
    return uint32_t(n < 8.0f ? 8.0f : (n > 64.0f ? 64.0f : n));
}

// Display range of the LDR stage: 1 for SDR, peak / paper white for HDR output.
inline float ldrRange(const FrameContext& ctx) {
    if (ctx.frame.display2.y < 0.5f) return 1.0f;
    float paper = ctx.frame.display.w > 1.0f ? ctx.frame.display.w : 200.0f;
    return ctx.frame.display2.x > paper ? ctx.frame.display2.x / paper : 1.0f;
}

// Fullscreen draw into several render targets (MRT) or with an explicit clear.
inline void drawFullscreenMRT(FrameContext& ctx, rhi::Pipeline* pipe, std::initializer_list<rhi::Texture*> targets,
                              const char* name, bool clear = false) {
    rhi::RenderPassDesc rp;
    for (rhi::Texture* t : targets) {
        rp.colors[rp.colorCount].texture = t;
        rp.colors[rp.colorCount].load = clear ? rhi::LoadOp::Clear : rhi::LoadOp::DontCare;
        ++rp.colorCount;
    }
    rp.name = name;
    ctx.cmd.beginRenderPass(rp);
    ctx.cmd.bindPipeline(pipe);
    ctx.cmd.bindUniformBuffer(0, ctx.frameUBO);
    ctx.cmd.draw(3);
    ctx.cmd.endRenderPass();
}

inline rhi::BufferPtr makeParams(Engine& e, size_t size, const char* name, const char* owner) {
    return e.device().createBuffer({.size = size, .usage = rhi::BufUsageUniform, .dynamic = true,
                                    .name = name, .owner = owner});
}

inline rhi::TextureDesc targetDesc(rhi::Format f, const char* name, const char* owner,
                                   uint32_t usage = rhi::TexUsageSampled | rhi::TexUsageRenderTarget) {
    rhi::TextureDesc d;
    d.format = f; d.usage = usage; d.name = name; d.owner = owner;
    return d;
}

}  // namespace gfx::aa
