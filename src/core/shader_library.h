// ShaderLibrary — loads GLSL from shaders/, resolves #include, injects defines, and hot-reloads.
// Programs are owned here; the Program* you get stays valid across reloads (backend swaps the
// binary in place, Program::generation() bumps). A failed reload keeps the last good binary and
// shows the compile log in the overlay.
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "rhi/rhi.h"

namespace gfx {

class ShaderLibrary {
public:
    void init(rhi::Device& device, std::filesystem::path shaderRoot);

    // Returns a cached program for an identical desc (files + defines).
    rhi::Program* get(const rhi::ProgramDesc& desc);
    rhi::Program* graphics(const std::string& vs, const std::string& fs, std::vector<std::string> defines = {});
    rhi::Program* compute(const std::string& cs, std::vector<std::string> defines = {});

    // Global defines added to every program (e.g. "MSAA_SAMPLES=4"); changing them reloads all.
    void setGlobalDefine(const std::string& name, const std::string& value);
    void removeGlobalDefine(const std::string& name);

    // Polls file timestamps (called by the shader hot-reload feature) and reloads changed programs.
    int  checkForChanges();
    void reloadAll();

    const std::filesystem::path& root() const { return root_; }
    std::string lastError() const { return lastError_; }
    size_t programCount() const { return programs_.size(); }
    uint32_t reloadCount() const { return reloadCount_; }
    double lastCompileMs() const { return lastCompileMs_; }

    // Preprocess a single stage file (exposed for tools / D3D11 cross compilation).
    std::string preprocess(const std::string& file, const std::vector<std::string>& defines,
                           std::set<std::string>* dependencies = nullptr) const;

private:
    struct Entry {
        rhi::ProgramPtr program;
        rhi::ProgramDesc desc;
        std::set<std::string> deps;   // absolute file paths
        std::filesystem::file_time_type newest{};
    };
    bool build(Entry& e);
    std::vector<std::string> allDefines(const std::vector<std::string>& local) const;

    rhi::Device* device_ = nullptr;
    std::filesystem::path root_;
    std::map<std::string, Entry> programs_;
    std::map<std::string, std::string> globalDefines_;
    std::string lastError_;
    uint32_t reloadCount_ = 0;
    double lastCompileMs_ = 0.0;
};

}  // namespace gfx
