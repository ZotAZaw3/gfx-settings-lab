# gfx-settings-lab — Module API (v1)

This is the contract every feature module builds against. The foundation thread owns
`src/core/`, `src/rhi/`, `shaders/common/`, `shaders/core/` and this file. If you need something
that isn't here, message the foundation thread rather than editing core files; small additions
land quickly.

Repo: https://github.com/ZotAZaw3/gfx-settings-lab (`main` builds and runs; the shared mirror
`/mnt/project-files/gfxlab/` is only a scratch copy now).

Jitter convention: `Camera::projection` applies `P' = translate(2*jx/w, -2*jy/h, 0) * P`; jitter is in
pixels with +x right and +y **down** (uv space), so a positive jitter shifts the rasterized image by
+jitter pixels in uv space.

---

## 1. Toolchain and conventions

| Thing | Choice |
|---|---|
| Language | C++20 (C++17 code is fine), CMake ≥ 3.20 |
| Window / input | GLFW 3.4 |
| UI | Dear ImGui 1.91 (settings menu is generated; you rarely touch ImGui) |
| Math | glm 1.0 (`GLM_FORCE_DEPTH_ZERO_TO_ONE`, right-handed, `GLM_FORCE_RADIANS`) |
| Backends | OpenGL 4.5 core (primary) and Direct3D 11 (secondary). No D3D12 / Vulkan. |
| Shaders | GLSL 450, explicit `layout(binding = N)`, `#include "path"` relative to `shaders/`. D3D11 cross-compiles the same GLSL (glslang → SPIR-V → SPIRV-Cross HLSL), so **never write HLSL**. |
| Clip space | D3D style everywhere: NDC z ∈ [0,1], **uv (0,0) = top-left** of every texture and target. GL enforces this with `glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE)`. |
| Depth | **Reversed-Z, infinite far plane.** Near = 1, far = 0. Clear to 0, compare `GREATER`. Format D32F. `linearDepth(d) = near / d`. |
| Units | meters, Y up, right-handed. Light intensities are relative (sun ≈ 6, exposure in FrameConstants). |
| Velocity | `scene.velocity` RG16F = NDC(current) − NDC(previous), unjittered. `prevUV = uv - v * vec2(0.5, -0.5)`. |
| GPU access | **Only through `gfx::rhi`** (`src/rhi/rhi.h`). No raw `gl*` calls in `src/modules/**`, or D3D11 breaks. |

Shader rules that keep the D3D11 cross-compile working:
- all uniforms live in `std140` uniform blocks (no loose `uniform float x;`), pad members to `vec4`;
- SSBOs are `std430`; images need an explicit format qualifier (`layout(rgba16f, binding=0)`);
- use `texture`, `textureLod`, `texelFetch`, `imageLoad/Store`; avoid `gl_FragCoord.w` tricks and
  GL-only built-ins beyond `gl_VertexID`, `gl_InstanceID`, `gl_FragCoord`, `gl_GlobalInvocationID`
  and friends.

## 2. Folder ownership

```
src/rhi/                 foundation (rhi.h + gl/ + d3d11/)
src/core/                foundation (engine, settings, passes, scene, renderer, profiler, menu)
src/modules/core/        foundation features (textures, models, shaders, vsync, display, latency, render scale, overlay)
src/modules/aa/          AA + upscaling + frame generation thread
src/modules/lighting/    shadows / AO / reflections / SSS / GI / PBR thread
src/modules/raytracing/  ray tracing / path tracing thread
src/modules/geometry/    geometry / LOD / tessellation / particles / VFX thread
src/modules/post/        post-processing thread
src/modules/audio/       audio thread
shaders/<same names>/    shaders per area; shaders/common/ is shared, read-only for modules
docs/<setting>.md        one doc per setting (template: docs/_TEMPLATE.md)
assets/                  scenes (glTF), textures
```

Any `.cpp` under `src/modules/**` is compiled automatically (CMake `GLOB_RECURSE ... CONFIGURE_DEPENDS`).
Shared third-party code goes in `third_party/<lib>/` with its own small CMake target.

## 3. Writing a module

```cpp
#include "core/engine.h"   // pulls in module.h, frame.h, settings.h, renderer.h, scene.h, rhi.h

class SsaoModule : public gfx::Module {
public:
    const char* name() const override { return "lighting.ssao"; }

    void registerSettings(gfx::SettingsRegistry& s) override {
        mode_ = &s.addEnum({.id = "lighting.ao.mode", .label = "Ambient Occlusion",
                            .category = "Lighting", .group = "Ambient Occlusion",
                            .tooltip = "Darkens creases and contact areas where ambient light is blocked.",
                            .doc = "ambient_occlusion"},
                           {"Off", "SSAO", "HBAO", "GTAO"}, 3)
                    .presets(0, 1, 2, 3);
        radius_ = &s.addFloat({.id = "lighting.ao.radius", .label = "AO Radius", .category = "Lighting",
                               .group = "Ambient Occlusion", .doc = "ambient_occlusion"}, 0.5f, 0.05f, 3.0f)
                      .visible([this] { return mode_->asInt() != 0; });
    }

    void init(gfx::Engine& e) override {
        pipeline_ = e.createFullscreenPipeline("lighting/ssao.frag");
        params_ = e.device().createBuffer({.size = sizeof(Params), .usage = gfx::rhi::BufUsageUniform,
                                           .name = "ssao.params", .owner = name()});
    }

    void resize(gfx::Engine& e, gfx::Extent render, gfx::Extent) override {
        // Named, auto-resized target other modules can read ("ao.result")
        ao_ = e.targets().create("lighting.ao", {.format = gfx::rhi::Format::R8,
                                 .usage = gfx::rhi::TexUsageSampled | gfx::rhi::TexUsageRenderTarget,
                                 .name = "lighting.ao", .owner = name()}, gfx::TargetScale::Render);
    }

    void addPasses(gfx::PassList& p) override {
        p.add(gfx::Stage::Lighting, "SSAO", [this](gfx::FrameContext& ctx) {
            if (ctx.replaceLighting) return;
            ctx.cmd.updateBuffer(params_.get(), Params{radius_->asFloat()});
            ctx.cmd.bindUniformBuffer(3, params_.get());
            ctx.cmd.bindTexture(0, ctx.targets.get("scene.depth"), ctx.engine.pointClamp());
            ctx.cmd.bindTexture(1, ctx.targets.get("gbuffer.normal"), ctx.engine.pointClamp());
            ctx.fullscreen(pipeline_.get(), ao_, "SSAO");
        }, /*order*/ 10.0f, [this] { return mode_->asInt() != 0; });
    }
private:
    struct Params { float radius; float pad[3]; };
    gfx::Setting* mode_ = nullptr; gfx::Setting* radius_ = nullptr;
    gfx::rhi::PipelinePtr pipeline_; gfx::rhi::BufferPtr params_;
    gfx::rhi::Texture* ao_ = nullptr;
};
GFX_REGISTER_MODULE(SsaoModule);
```

### Module hooks (`src/core/module.h`)

| Hook | When | Use |
|---|---|---|
| `name()` | — | `"<area>.<feature>"`; also the owner tag for VRAM / GPU / CPU attribution |
| `priority()` | — | init/update/filter order (core features use negative values) |
| `registerSettings(SettingsRegistry&)` | once, before init | add settings |
| `init(Engine&)` | once | create pipelines, buffers, samplers |
| `resize(Engine&, render, output)` | start + whenever render/output size changes, or a `SettingRequiresResize` setting changes | (re)create size-dependent resources |
| `addPasses(PassList&)` | once after init | register passes into stages |
| `update(FrameContext&)` | every frame before passes | CPU work: simulation, audio listener, streaming requests |
| `filterDraws(FrameContext&, DrawList&)` | every frame in FrameSetup | culling, LOD selection, instancing (geometry) |
| `bindShadingResources(FrameContext&)` | before any pass that includes `lighting/shade.glsl` | lighting binds shadow maps, probes, LUTs |
| `onGui(FrameContext&)` | every frame in UI | optional debug window |
| `cpuMemoryBytes()` | profiler | report CPU-side memory you hold |
| `shutdown(Engine&)` | exit | release |

`Engine` accessors: `device()`, `settings()`, `shaders()`, `scene()`, `renderer()`, `targets()`,
`profiler()`, `camera()`, `renderSize()`, `outputSize()`, `windowFocused()`, `frameIndex()`,
`findModule(name)`, `createFullscreenPipeline(fs, defines, blend)`, `linearClamp()`,
`pointClamp()`, `linearRepeat()`.

## 4. Stages and the frame

Passes run in stage order, then ascending `order`. Each pass is wrapped in a GPU timestamp pair and
a CPU timer automatically. FrameConstants are bound at UBO 0 before every pass.

| # | Stage | Resolution | Who | What |
|---|---|---|---|---|
| 0 | `FrameSetup` | — | core, aa, geometry, rt | camera, **jitter** (`ctx.camera.setJitter(px)` before core finalizes FrameConstants at order 1000), draw list, BVH refit, streaming |
| 1 | `Shadow` | shadow res | lighting | shadow maps via `ctx.renderer.drawScene({.mode = DepthOnly, .viewProj = cascadeVP, .pipeline = yours})` |
| 2 | `DepthPrepass` | render | core | optional |
| 3 | `GBuffer` | render | core (+ geometry) | writes `gbuffer.*`, `scene.velocity`, `scene.depth`; emissive goes to `gbuffer.emissive` |
| 4 | `Lighting` | render | lighting, rt | AO, SSR, GI, RT, **deferred resolve → `scene.color`** |
| 5 | `Forward` | render | core | used instead of GBuffer+resolve when MSAA > 1 (`ctx.msaaForward`), shading via `lighting/shade.glsl` |
| 6 | `Translucent` | render | geometry | particles, fog, clouds, transparent meshes. Read `scene.depth`, blend into `scene.color` |
| 7 | `PostRender` | render | post | DOF, motion blur (ping-pong via `ctx.post`) |
| 8 | `Upscale` | render → output | aa | TAA / temporal upscaler / spatial. `ctx.post.dst()` is output-res here. If nobody swaps, core upsamples with the "Upscale filter" setting. A temporal upscaler sets `engine.upscalerRenderScale` (> 0) + `engine.requestResize = true`; core then uses it instead of "Render Resolution" and dynamic resolution multiplies on top |
| 9 | `PostOutput` | output | post | bloom, god rays, lens flare / sun glare, auto exposure |
| 10 | `ToneMap` | output | post | HDR → display-referred linear (SDR: 0..1; HDR: nits / paper white) |
| 11 | `PostLDR` | output | post, aa | vignette, grain, CA, grading, CAS sharpening, FXAA/SMAA on LDR |
| 12 | `OutputTransform` | output | core | brightness, gamma / sRGB, HDR10 PQ / scRGB → `final.output` |
| 13 | `FrameGen` | output | aa | reads `final.output` history; set `ctx.interpolatedFrame` to present an extra frame first |
| 14 | `UI` | backbuffer | core | ImGui menu + overlay (frame gen never sees UI) |

If ToneMap has no pass (post module absent) core applies a plain ACES fit so the image stays sane.

### Post chain (`ctx.post`)
```cpp
auto* in  = ctx.post.src();   // current HDR image (starts as scene.color)
auto* out = ctx.post.dst();   // scratch at this stage's resolution (RGBA16F)
ctx.cmd.bindTexture(0, in, ctx.engine.linearClamp());
ctx.fullscreen(pipeline, out, "Bloom composite");
ctx.post.swap();              // out becomes src for the next pass
```

### Lighting replacement and lighting inputs
- **Path tracing** sets `ctx.replaceLighting = true` in `FrameSetup`; every raster lighting pass
  (resolve, SSAO, SSR, shadows) must return early when it is true. The path tracer writes `scene.color` in `Lighting`.
- **Shared per-pixel lighting inputs** (render res). The module that produces one creates it with
  `ctx.targets.create(name, ...)`; the deferred resolve (lighting thread) and the forward path read
  them when `ctx.targets.get(name) != nullptr`, otherwise they use the fallback.

| Target | Format | Meaning | Fallback | Writers |
|---|---|---|---|---|
| `lighting.sunVisibility` | R8 / R16F | 1 = sun visible | shadow maps sampled in resolve | shadow maps (screen-space resolve), RT shadows |
| `lighting.ao` | R8 | multiplies **ambient/indirect only** | 1 | SSAO / HBAO / GTAO / RTAO |
| `lighting.indirectDiffuse` | RGBA16F | irradiance, replaces probe/flat ambient | probes / flat ambient | SSGI, RTGI |
| `lighting.indirectSpecular` | RGBA16F | radiance, a = confidence; replaces env specular where a > 0 | env cubemap | SSR, RT reflections |
| `lighting.contactShadows` | R8 | multiplies direct sun | 1 | contact shadows |

## 5. Render targets (`ctx.targets`)

| Name | Format | Size | Contents |
|---|---|---|---|
| `scene.color` | RGBA16F | render | HDR lit color. Resolve writes it; translucent blends into it |
| `scene.depth` | D32F | render | reversed-Z, sampleable (`texture(...).r`) |
| `scene.velocity` | RG16F | render | NDC(cur) − NDC(prev), unjittered; sky = camera-only motion |
| `gbuffer.albedo` | RGBA8_SRGB | render | rgb base color (linear when sampled), a = specular occlusion (1 default) |
| `gbuffer.normal` | RGBA16F | render | xyz world normal (normalized), w = shading model id (0 default, 1 subsurface, 2 foliage, 3 unlit, 4 clearcoat) |
| `gbuffer.material` | RGBA8 | render | r roughness, g metallic, b material AO, a reflectance (0.5 = F0 0.04) |
| `gbuffer.emissive` | RGBA16F | render | rgb emissive radiance, a = SSS mask / thickness |
| `final.output` | RGBA16F | output | display-encoded frame after OutputTransform (frame gen input) |

`ctx.targets.create(name, desc, TargetScale::Render | Output | Fixed, scale)` creates a target that
core resizes for you (e.g. half-res: `scale = 0.5f`). `get(name)` returns `nullptr` if absent.
With MSAA on, `scene.color`/`scene.depth` are the **resolved** single-sample textures by the time
`Translucent` runs.

## 6. Binding slots

| Kind | Slots | Owner |
|---|---|---|
| UBO | 0 FrameConstants · 1 DrawConstants · 2 core reserved | core |
| UBO | 3–5 per-pass params (anyone) · 6–7 lighting shade resources · 8–13 free (D3D11 max is 14) | modules |
| SSBO | 0 lights · 1 materials · 2 vertices · 3 indices (when bound by core/RT) | core |
| SSBO | 4–7 lighting shade resources · 8–15 free (RT uses 10–13) | modules |
| Texture | 0–5 material maps in scene draws (else free) · 6–7 free · 8–15 lighting shade resources | — |
| Image | 0–7 free per pass | — |

Slots only collide inside one pass: you're free to use 0–5 for your own fullscreen pass.
Avoid 8–15 / UBO 6–7 / SSBO 4–7 only in shaders that include `lighting/shade.glsl`.

## 7. Scene and drawing

`Scene` (`src/core/scene.h`) keeps CPU copies **and** GPU buffers:
- `vertices` (`Vertex`: position, normal, tangent, uv — 48 bytes), `indices` (uint32), one shared
  vertex buffer + index buffer (also bindable as SSBO 2 / 3 for vertex pulling and BVH builds);
- `primitives` (baseVertex, vertexCount, `lods[]` index ranges, material, local AABB),
  `meshes`, `instances` (world, prevWorld, world AABB), `materials` (+ `materialBuffer` SSBO 1),
  `lights` (+ `lightBuffer` SSBO 0), `sun`, `textures` (with streaming state);
- `geometryVersion()` bumps when geometry changes (rebuild BVH); `addLod(prim, indices, error)` appends LODs.

`DrawList` (`ctx.draws`): `opaque` / `transparent` arrays of `{instance, primitive, lod, distance}`.
Core fills it with every primitive at LOD0 at the start of `FrameSetup` (order −100), then calls
every module's `filterDraws` in priority order (the geometry module culls, picks LOD, and can
append items). Everyone who draws the scene (G-buffer, forward, shadows, custom) consumes the list
through `ctx.renderer.drawScene(ctx, params)`:

```cpp
gfx::SceneDrawParams p;
p.mode = gfx::DrawMode::DepthOnly;     // DepthOnly, GBuffer, Forward, Custom
p.viewProj = cascadeViewProj;
p.pipeline = myShadowPipeline.get();   // must include "common/draw.glsl" and use DrawUBO
p.shadowCastersOnly = true;
ctx.renderer.drawScene(ctx, p);
```
`shaders/common/surface.glsl` holds `Surface`, the `SHADING_*` ids, `GpuLight` and the light SSBO (binding 0)
and declares no samplers, so compute / fullscreen passes can include it. Light count is
`int(uCounts.x)` in FrameConstants (y = instances, z = materials).

Custom scene shaders use `shaders/common/draw.glsl` (DrawUBO, GpuMaterial SSBO, material samplers
0–5, `Surface` struct) and vertex inputs at locations 0 position, 1 normal, 2 tangent, 3 uv.

**Shared shading function.** The lighting thread owns `shaders/lighting/shade.glsl`, which must
provide `vec3 shadeSurface(Surface s, vec3 V, vec2 uv)`. Core ships a simple Lambert + GGX
placeholder at that path so the app runs before lighting lands; the lighting thread overwrites it.

## 8. Settings (`src/core/settings.h`)

```cpp
gfx::Setting& s = reg.addEnum({.id = "aa.method", .label = "Anti-Aliasing", .category = "Anti-Aliasing & Upscaling",
                               .group = "Anti-Aliasing", .tooltip = "...", .doc = "anti_aliasing",
                               .flags = gfx::SettingRequiresResize}, {"Off", "FXAA", "SMAA", "TAA"}, 3)
                  .presets(1, 1, 3, 3)                  // Low, Medium, High, Ultra
                  .visible([&] { ... })                 // hide
                  .enabled([&] { ... })                 // grey out (unsupported)
                  .changed([&](gfx::Setting&) { ... }); // callback
s.asInt(); s.asBool(); s.asFloat();                     // read every frame (cheap)
```
- Types: `addBool`, `addInt(def, min, max)`, `addFloat(def, min, max)`, `addEnum(options, def)`.
- Flags: `SettingRequiresResize` (core calls every module's `resize`), `SettingRequiresReload`
  (shaders rebuilt), `SettingRequiresRestart`, `SettingNoPreset`, `SettingDebug`.
- Categories (tab order): Display, Graphics, Textures, Lighting, Shadows, Ray Tracing,
  Anti-Aliasing & Upscaling, Geometry, World, Effects, Characters, Physics, Post Processing,
  Latency, Performance, Audio, Debug. New names are appended as extra tabs.
- Change any setting from code with `settings.set("id", value)` (clamps, fires callbacks, informs the profiler).
  `Setting::setNumber` is a raw write without callbacks.
- Presets Low / Medium / High / Ultra apply `presets(...)` values; touching any setting flips the
  preset to Custom. Values persist to `settings.ini`.
- The menu shows each setting's tooltip, a "?" button that opens `docs/<doc>.md` in a side panel,
  and the **measured cost** (Δ GPU ms, Δ CPU ms, Δ VRAM, Δ RAM) from the profiler.

## 9. Profiling: CPU / GPU / RAM / VRAM for every setting

Automatic, no work needed:
- Every pass: GPU ms (timestamp queries) and CPU ms, listed in the overlay grouped by module.
- Every `rhi` texture/buffer is tagged with `owner` (pass your module name in `TextureDesc::owner`
  / `BufferDesc::owner`) → VRAM per module.
- Every setting change: the profiler averages 60 frames before vs 60 frames after (15 warm-up frames
  skipped) and stores Δ GPU ms, Δ CPU ms, Δ frame ms, Δ VRAM MB, Δ RAM MB on that setting. The
  menu shows it inline.
- `gfxlab --bench-settings docs/_measurements` sweeps every value of every setting (from the High
  preset baseline) and writes `docs/_measurements/<gpu>.md` and `.csv`.

Optional:
- `GFX_CPU_SCOPE(ctx.profiler, "BVH build");` for sub-pass CPU timing.
- `ctx.profiler.counter("particles.alive", n);` custom stats in the overlay.
- `size_t cpuMemoryBytes() const override` to report CPU memory (BVH nodes, particle arrays).

**Docs must quote measured numbers** in the Optimization section: a table of Δ GPU ms, Δ CPU ms,
Δ VRAM, Δ RAM per value at 1920×1080, naming the GPU/driver. Numbers measured in the cloud run on
Mesa llvmpipe (software rasterizer); label them as such since they only show relative cost.

## 10. Docs

One file per setting: `docs/<setting>.md` from `docs/_TEMPLATE.md` with the five sections
**Definition, How it works, Breakdown, Optimization, Rebuild**. The `doc` field of a setting
points at it; the in-app panel renders it.

## 11. Building and running

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release        # fetches glfw, imgui, glm
cmake --build build -j
./build/gfxlab                                         # procedural demo scene
./build/gfxlab --scene assets/Sponza/Sponza.gltf
./build/gfxlab --backend d3d11                         # Windows
./build/gfxlab --frames 120 --screenshot out.png       # CI / headless (xvfb-run)
./build/gfxlab --bench-settings docs/_measurements
```
Keys: F1 settings menu, F2 overlay, F5 reload shaders, F12 screenshot, RMB + WASD/QE fly, Shift fast.

## 12. Contributing

Work on a branch `<area>/<feature>` and open a PR against `main`. Touch only your folders plus
`docs/`. Core changes go through the foundation thread.
