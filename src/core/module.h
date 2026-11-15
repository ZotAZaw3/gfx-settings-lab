// gfx::Module — the unit every feature thread implements.
//
//   class SsaoModule : public gfx::Module {
//   public:
//       const char* name() const override { return "lighting.ssao"; }
//       void registerSettings(gfx::SettingsRegistry& s) override { ... }
//       void init(gfx::Engine& e) override { ... create pipelines ... }
//       void resize(gfx::Engine& e, gfx::Extent render, gfx::Extent output) override { ... }
//       void addPasses(gfx::PassList& p) override {
//           p.add(gfx::Stage::Lighting, "SSAO", [this](gfx::FrameContext& ctx) { run(ctx); }, 10.0f,
//                 [this] { return enabled_->asBool(); });
//       }
//   };
//   GFX_REGISTER_MODULE(SsaoModule);
//
// Any .cpp under src/modules/** is compiled into the app automatically (CMake glob), and the
// static registrar adds the module — no central list to edit.
#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "core/frame.h"

namespace gfx {

class Engine;
class SettingsRegistry;
struct DrawList;

class Module {
public:
    virtual ~Module() = default;

    // Stable id, also the owner tag for VRAM/RAM/GPU attribution: "<area>.<feature>".
    virtual const char* name() const = 0;
    // Lower runs first for init/update/filterDraws (core features use -100..-1).
    virtual int priority() const { return 0; }

    virtual void registerSettings(SettingsRegistry&) {}
    virtual void init(Engine&) {}
    virtual void resize(Engine&, Extent /*render*/, Extent /*output*/) {}
    virtual void addPasses(PassList&) {}
    virtual void shutdown(Engine&) {}

    // CPU work every frame before any pass (input, simulation, audio listener, streaming requests).
    virtual void update(FrameContext&) {}

    // Geometry module: cull / pick LOD / add instances to the draw list core renders.
    virtual void filterDraws(FrameContext&, DrawList&) {}

    // Lighting module: bind shadow maps, AO, probes, BRDF LUT... (texture slots 8-15,
    // UBO 6-7, SSBO 4-7) before any pass that includes shaders/lighting/shade.glsl.
    virtual void bindShadingResources(FrameContext&) {}

    // Optional extra ImGui window (debug views). The settings menu itself is automatic.
    virtual void onGui(FrameContext&) {}

    // Profiler: report CPU-side memory this module holds (bytes), e.g. BVH or particle arrays.
    virtual size_t cpuMemoryBytes() const { return 0; }
};

using ModuleFactory = std::function<std::unique_ptr<Module>()>;
std::vector<ModuleFactory>& moduleFactories();

struct ModuleRegistrar {
    explicit ModuleRegistrar(ModuleFactory f) { moduleFactories().push_back(std::move(f)); }
};

}  // namespace gfx

#define GFX_CONCAT_INNER(a, b) a##b
#define GFX_CONCAT(a, b) GFX_CONCAT_INNER(a, b)
#define GFX_REGISTER_MODULE(Type)                                                    \
    static ::gfx::ModuleRegistrar GFX_CONCAT(s_gfxModuleRegistrar_, __LINE__)(       \
        [] { return std::unique_ptr<::gfx::Module>(new Type()); })
