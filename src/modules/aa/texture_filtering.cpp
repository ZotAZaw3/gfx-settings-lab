// Texture filtering: Nearest / Bilinear / Trilinear / Anisotropic 2x-16x for every material texture,
// plus the negative mip bias that temporal upscalers and TAA need.  Doc: docs/texture_filtering.md
#include <algorithm>
#include <cmath>

#include "modules/aa/aa_util.h"

namespace gfx::aa {

class TextureFilteringModule : public Module {
public:
    const char* name() const override { return "aa.texture_filtering"; }

    void registerSettings(SettingsRegistry& s) override {
        mode_ = &s.addEnum({.id = "textures.filtering", .label = "Texture Filtering", .category = "Textures",
                            .group = "Filtering",
                            .tooltip = "How textures are sampled between texels and at a distance. Anisotropic keeps "
                                       "floors and roads sharp at grazing angles.",
                            .doc = "texture_filtering"},
                           {"Nearest", "Bilinear", "Trilinear", "Anisotropic 2x", "Anisotropic 4x",
                            "Anisotropic 8x", "Anisotropic 16x"}, 6)
                    .presets(2, 4, 6, 6);
        autoBias_ = &s.addBool({.id = "textures.filtering.upscale_bias", .label = "Mip Bias for Upscaling",
                                .category = "Textures", .group = "Filtering",
                                .tooltip = "Sample textures at output-resolution detail when TAA or a temporal "
                                           "upscaler is on (bias = log2(render / output)).",
                                .doc = "texture_filtering"}, true);
    }

    void init(Engine& e) override { maxAniso_ = std::max(1.0f, e.device().caps().maxAnisotropy); }

    void update(FrameContext& ctx) override {
        int mode = mode_->asInt();
        float bias = 0.0f;
        if (autoBias_->asBool()) {
            int up = ctx.settings.getInt(kUpscaler), method = ctx.settings.getInt(kMethod);
            bool temporal = up == UpTemporal || up >= UpDLSS || method == MethodTAA ||
                            ctx.settings.getInt(kRayRecon) == 1;
            if (temporal) {
                float ratio = float(ctx.renderSize.width) / float(std::max(1u, ctx.outputSize.width));
                bias = std::log2(std::min(ratio, 1.0f)) - (ratio >= 0.99f ? 0.5f : 0.0f);   // mild -0.5 for native TAA
            }
        }
        bias = std::round(bias * 8.0f) / 8.0f;   // avoid rebuilding the sampler for tiny DRS changes
        if (mode == lastMode_ && bias == lastBias_) return;
        lastMode_ = mode; lastBias_ = bias;

        rhi::SamplerDesc d;
        d.addressU = d.addressV = d.addressW = rhi::AddressMode::Repeat;
        d.mipLodBias = bias;
        switch (mode) {
            case 0: d.minFilter = d.magFilter = d.mipFilter = rhi::Filter::Nearest; break;
            case 1: d.minFilter = d.magFilter = rhi::Filter::Linear; d.mipFilter = rhi::Filter::Nearest; break;
            case 2: d.minFilter = d.magFilter = d.mipFilter = rhi::Filter::Linear; break;
            default:
                d.minFilter = d.magFilter = d.mipFilter = rhi::Filter::Linear;
                d.maxAnisotropy = std::min(maxAniso_, float(1 << (mode - 2)));   // 3->2x ... 6->16x
                break;
        }
        ctx.renderer.setMaterialSampler(ctx.device.createSampler(d));
    }

private:
    Setting* mode_ = nullptr; Setting* autoBias_ = nullptr;
    float maxAniso_ = 1.0f;
    int lastMode_ = -1; float lastBias_ = 1e9f;
};
GFX_REGISTER_MODULE(TextureFilteringModule);

}  // namespace gfx::aa
