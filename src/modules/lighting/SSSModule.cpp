// SSSModule ("lighting.sss") - subsurface scattering for skin, wax, marble, leaves:
//   wrap lighting (cheap) or separable screen-space SSS (Jimenez 2015) with a sum-of-Gaussians
//   skin profile, plus thin-object transmittance. Docs: docs/subsurface_scattering.md
#include <algorithm>
#include <cmath>
#include <vector>

#include "core/engine.h"
#include "modules/lighting/LightingShared.h"

namespace gfx::lighting {

class SSSModule : public Module {
public:
    const char* name() const override { return "lighting.sss"; }

    void registerSettings(SettingsRegistry& s) override {
        const char* cat = "Lighting";
        mode_ = &s.addEnum({.id = "lighting.sss.mode", .label = "Subsurface Scattering", .category = cat, .group = "Subsurface Scattering",
                            .tooltip = "Light scattering under the surface of skin, wax and leaves (soft, reddish shadow edges).",
                            .doc = "subsurface_scattering"}, {"Off", "Wrap lighting", "Screen-space (11 taps)", "Screen-space (17 taps)", "Screen-space (25 taps)"}, 3)
                    .presets(1, 2, 3, 4);
        transmit_ = &s.addBool({.id = "lighting.sss.transmittance", .label = "SSS Transmittance", .category = cat,
                                .group = "Subsurface Scattering", .tooltip = "Backlit glow through thin parts (ears, leaves).",
                                .doc = "subsurface_scattering"}, true)
                        .presets(0, 0, 1, 1)
                        .visible([this] { return mode_->asInt() != 0; });
        width_ = &s.addFloat({.id = "lighting.sss.width", .label = "Scatter Width", .category = cat, .group = "Subsurface Scattering",
                              .tooltip = "How far light travels under the surface (world units).", .doc = "subsurface_scattering",
                              .flags = SettingNoPreset}, 0.012f, 0.001f, 0.1f)
                     .fmt("%.3f m")
                     .visible([this] { return mode_->asInt() >= 2; });
        strength_ = &s.addFloat({.id = "lighting.sss.strength", .label = "SSS Strength", .category = cat, .group = "Subsurface Scattering",
                                 .tooltip = "0 = plain diffuse, 1 = full scattering.", .doc = "subsurface_scattering",
                                 .flags = SettingNoPreset}, 1.0f, 0.0f, 1.0f)
                        .visible([this] { return mode_->asInt() != 0; });
    }

    void init(Engine& e) override {
        blur_ = e.shaders().compute("lighting/sss_blur.comp");
        params_ = e.device().createBuffer({.size = sizeof(Params), .usage = rhi::BufUsageUniform, .name = "sss.params", .owner = name()});
    }

    void update(FrameContext& ctx) override {
        auto& S = shared();
        int m = mode_->asInt();
        static const int taps[5] = {0, 0, 11, 17, 25};
        separable_ = m >= 2 && !ctx.replaceLighting && !ctx.msaaForward;
        S.ubo.sssCfg = glm::ivec4(m == 0 ? 0 : (m == 1 ? 1 : 2), taps[m], transmit_->asBool() && m != 0 ? 1 : 0, 0);
        S.ubo.sssParams = glm::vec4(width_->asFloat(), strength_->asFloat(), 0.5f, 0.0f);
        if (separable_ && !diffuse_) {
            diffuse_ = ctx.targets.create("lighting.sssDiffuse", storageDesc(rhi::Format::RGBA16F, "lighting.sssDiffuse", name()));
            temp_ = ctx.targets.create("lighting.sssTemp", storageDesc(rhi::Format::RGBA16F, "lighting.sssTemp", name()));
        } else if (!separable_ && diffuse_) {
            ctx.targets.remove("lighting.sssDiffuse");
            ctx.targets.remove("lighting.sssTemp");
            diffuse_ = temp_ = nullptr;
        }
        if (diffuse_) {
            diffuse_ = ctx.targets.get("lighting.sssDiffuse");
            temp_ = ctx.targets.get("lighting.sssTemp");
        }
        S.sssDiffuse = diffuse_;
        if (taps[m] != kernelTaps_ || strength_->asFloat() != kernelStrength_) buildKernel(taps[m], strength_->asFloat());
    }

    void addPasses(PassList& p) override {
        p.add(Stage::Lighting, "Subsurface scattering", [this](FrameContext& ctx) { run(ctx); }, 110.0f,
              [this] { return separable_ && diffuse_ != nullptr; });
    }

private:
    struct Params {
        glm::vec4 dir;
        glm::ivec4 cfg;
        glm::vec4 kernel[25];
    };

    // Separable SSS kernel (Jimenez): offsets distributed as sign*|x|^2 over [-range, range], weights
    // are the skin diffusion profile integrated over each tap's interval. Tap 0 is the center.
    void buildKernel(int n, float strength) {
        kernelTaps_ = n;
        kernelStrength_ = strength;
        if (n == 0) return;
        const float range = n > 20 ? 3.0f : 2.0f;
        const float exponent = 2.0f;
        std::vector<glm::vec4> k(n);
        float step = 2.0f * range / float(n - 1);
        for (int i = 0; i < n; ++i) {
            float o = -range + float(i) * step;
            float sign = o < 0.0f ? -1.0f : 1.0f;
            k[i].w = range * sign * std::abs(std::pow(o, exponent)) / std::pow(range, exponent);
        }
        const glm::vec3 falloff(1.0f, 0.37f, 0.3f);   // red scatters furthest in skin
        auto gaussian = [](float v, float r, glm::vec3 fo) {
            glm::vec3 rr = r / (glm::vec3(0.001f) + fo);
            return glm::exp(-(rr * rr) / (2.0f * v)) / std::sqrt(2.0f * 3.14159265f * v);
        };
        auto profile = [&](float r) {   // d'Eon & Luebke skin fit (sum of 5 Gaussians)
            return 0.100f * gaussian(0.0484f, r, falloff) + 0.118f * gaussian(0.187f, r, falloff) +
                   0.113f * gaussian(0.567f, r, falloff) + 0.358f * gaussian(1.99f, r, falloff) +
                   0.078f * gaussian(7.41f, r, falloff);
        };
        for (int i = 0; i < n; ++i) {
            float w0 = i > 0 ? std::abs(k[i].w - k[i - 1].w) : 0.0f;
            float w1 = i < n - 1 ? std::abs(k[i].w - k[i + 1].w) : 0.0f;
            glm::vec3 w = profile(k[i].w) * ((w0 + w1) / 2.0f);
            k[i] = glm::vec4(w, k[i].w);
        }
        glm::vec3 sum(0.0f);
        for (auto& v : k) sum += glm::vec3(v);
        for (auto& v : k) v = glm::vec4(glm::vec3(v) / sum, v.w);
        // move the center tap to index 0
        int c = n / 2;
        glm::vec4 center = k[c];
        for (int i = c; i > 0; --i) k[i] = k[i - 1];
        k[0] = center;
        // strength: lerp toward an identity kernel
        k[0] = glm::vec4(glm::mix(glm::vec3(1.0f), glm::vec3(k[0]), strength), k[0].w);
        for (int i = 1; i < n; ++i) k[i] = glm::vec4(glm::vec3(k[i]) * strength, k[i].w);
        for (int i = 0; i < 25; ++i) params_cpu_.kernel[i] = i < n ? k[i] : glm::vec4(0.0f);
    }

    void run(FrameContext& ctx) {
        if (ctx.replaceLighting) return;
        auto& cmd = ctx.cmd;
        rhi::Sampler* pt = ctx.engine.pointClamp();
        rhi::Texture* color = ctx.targets.get("scene.color");
        cmd.bindComputeProgram(blur_);
        cmd.bindTexture(3, ctx.targets.get("scene.depth"), pt);
        cmd.bindTexture(1, ctx.targets.get("gbuffer.normal"), pt);
        // H: sssDiffuse -> temp
        params_cpu_.dir = glm::vec4(1, 0, 0, 0);
        params_cpu_.cfg = glm::ivec4(kernelTaps_, 0, 0, 0);
        cmd.updateBuffer(params_.get(), params_cpu_);
        cmd.bindUniformBuffer(3, params_.get());
        cmd.bindTexture(5, diffuse_, ctx.engine.linearClamp());
        cmd.bindImage(0, temp_, 0, rhi::Access::Write);
        cmd.dispatch(groups(ctx.renderSize.width, 8), groups(ctx.renderSize.height, 8));
        cmd.barrier(rhi::BarrierTextureFetch | rhi::BarrierImage);
        // V: temp -> added into scene.color
        params_cpu_.dir = glm::vec4(0, 1, 0, 0);
        params_cpu_.cfg = glm::ivec4(kernelTaps_, 1, 0, 0);
        cmd.updateBuffer(params_.get(), params_cpu_);
        cmd.bindUniformBuffer(3, params_.get());
        cmd.bindTexture(5, temp_, ctx.engine.linearClamp());
        cmd.bindImage(0, color, 0, rhi::Access::ReadWrite);
        cmd.dispatch(groups(ctx.renderSize.width, 8), groups(ctx.renderSize.height, 8));
        cmd.barrier(rhi::BarrierTextureFetch | rhi::BarrierImage);
    }

    Setting *mode_ = nullptr, *transmit_ = nullptr, *width_ = nullptr, *strength_ = nullptr;
    rhi::Program* blur_ = nullptr;
    rhi::BufferPtr params_;
    Params params_cpu_{};
    rhi::Texture *diffuse_ = nullptr, *temp_ = nullptr;
    int kernelTaps_ = -1;
    float kernelStrength_ = -1.0f;
    bool separable_ = false;
};

GFX_REGISTER_MODULE(SSSModule);

}  // namespace gfx::lighting
