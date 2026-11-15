// gfx-settings-lab: one app, every graphics setting, each with docs and measured cost.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/engine.h"

static void usage() {
    std::printf(
        "gfxlab [options]\n"
        "  --scene <file.gltf>        load a glTF scene (default: procedural demo)\n"
        "  --backend gl|d3d11         rendering backend (d3d11: Windows builds only)\n"
        "  --size WxH                 window size (default 1600x900)\n"
        "  --frames N                 exit after N frames\n"
        "  --screenshot out.png       save the last frame (with --frames)\n"
        "  --bench-settings <dir>     measure every value of every setting, write <dir>/<gpu>.md/.csv\n"
        "  --settings <file.ini>      settings file (default settings.ini)\n"
        "  --hidden                   no visible window (CI / xvfb)\n");
}

int main(int argc, char** argv) {
    gfx::EngineConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--scene") cfg.scenePath = next();
        else if (a == "--backend") cfg.backend = next() == "d3d11" ? gfx::rhi::Backend::D3D11 : gfx::rhi::Backend::OpenGL;
        else if (a == "--size") { std::string s = next(); std::sscanf(s.c_str(), "%ux%u", &cfg.width, &cfg.height); }
        else if (a == "--frames") cfg.maxFrames = std::atoi(next().c_str());
        else if (a == "--screenshot") cfg.screenshotPath = next();
        else if (a == "--bench-settings") cfg.benchSettingsDir = next();
        else if (a == "--settings") cfg.settingsPath = next();
        else if (a == "--shaders") cfg.shaderRoot = next();
        else if (a == "--hidden") cfg.hidden = true;
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 1; }
    }
    gfx::Engine engine;
    if (!engine.init(cfg)) return 1;
    engine.run();
    engine.shutdown();
    return 0;
}
