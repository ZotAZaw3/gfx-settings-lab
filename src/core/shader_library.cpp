#include "core/shader_library.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace gfx {

namespace fs = std::filesystem;

void ShaderLibrary::init(rhi::Device& device, fs::path shaderRoot) {
    device_ = &device;
    root_ = std::move(shaderRoot);
}

static std::string readFile(const fs::path& p, bool* ok) {
    std::ifstream in(p, std::ios::binary);
    if (!in) { *ok = false; return {}; }
    std::stringstream ss; ss << in.rdbuf();
    *ok = true;
    return ss.str();
}

static std::string keyFor(const rhi::ProgramDesc& d, const std::vector<std::string>& defines) {
    std::string k = d.vs + "|" + d.tcs + "|" + d.tes + "|" + d.gs + "|" + d.fs + "|" + d.cs + "|";
    for (auto& def : defines) k += def + ";";
    return k;
}

std::vector<std::string> ShaderLibrary::allDefines(const std::vector<std::string>& local) const {
    std::vector<std::string> out;
    for (auto& [k, v] : globalDefines_) out.push_back(v.empty() ? k : k + "=" + v);
    out.insert(out.end(), local.begin(), local.end());
    return out;
}

// Recursive #include with include guards handled by the files themselves (#ifndef).
// Emits #line directives so compile errors point at the right file/line (file index -> name list in log).
static bool expand(const fs::path& root, const std::string& file, std::string& out, std::set<std::string>* deps,
                   std::vector<std::string>& stack, std::string& err, int depth) {
    if (depth > 32) { err = "include depth > 32 at " + file; return false; }
    fs::path path = root / file;
    bool ok = false;
    std::string src = readFile(path, &ok);
    if (!ok) { err = "cannot open shader file: " + path.string(); return false; }
    if (deps) deps->insert(fs::absolute(path).string());
    int fileIndex = int(stack.size());
    stack.push_back(file);
    std::istringstream in(src);
    std::string line;
    int lineNo = 0;
    out += "#line 1 " + std::to_string(fileIndex) + "\n";
    while (std::getline(in, line)) {
        ++lineNo;
        size_t p = line.find_first_not_of(" \t");
        if (p != std::string::npos && line.compare(p, 8, "#include") == 0) {
            size_t a = line.find('"', p), b = line.find('"', a + 1);
            if (a == std::string::npos || b == std::string::npos) { err = file + ":" + std::to_string(lineNo) + ": bad #include"; return false; }
            std::string inc = line.substr(a + 1, b - a - 1);
            if (!expand(root, inc, out, deps, stack, err, depth + 1)) return false;
            out += "#line " + std::to_string(lineNo + 1) + " " + std::to_string(fileIndex) + "\n";
            continue;
        }
        if (p != std::string::npos && line.compare(p, 8, "#version") == 0) continue;   // re-emitted at top
        out += line;
        out += '\n';
    }
    return true;
}

std::string ShaderLibrary::preprocess(const std::string& file, const std::vector<std::string>& defines,
                                      std::set<std::string>* deps) const {
    std::string body, err;
    std::vector<std::string> stack;
    if (!expand(root_, file, body, deps, stack, err, 0)) {
        const_cast<ShaderLibrary*>(this)->lastError_ = err;
        return {};
    }
    std::string out = "#version 450 core\n";
    out += device_->caps().backend == rhi::Backend::OpenGL ? "#define GFX_GL 1\n" : "#define GFX_D3D11 1\n";
    for (auto& d : defines) {
        auto eq = d.find('=');
        out += "#define " + (eq == std::string::npos ? d : d.substr(0, eq) + " " + d.substr(eq + 1)) + "\n";
    }
    out += "// files: ";
    for (size_t i = 0; i < stack.size(); ++i) out += std::to_string(i) + "=" + stack[i] + " ";
    out += "\n";
    out += body;
    return out;
}

bool ShaderLibrary::build(Entry& e) {
    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<std::string> defs = allDefines(e.desc.defines);
    rhi::ProgramSources src;
    e.deps.clear();
    lastError_.clear();
    auto stage = [&](const std::string& f, std::string& outSrc) -> bool {
        if (f.empty()) return true;
        outSrc = preprocess(f, defs, &e.deps);
        return !outSrc.empty();
    };
    bool ok = stage(e.desc.vs, src.vs) && stage(e.desc.tcs, src.tcs) && stage(e.desc.tes, src.tes) &&
              stage(e.desc.gs, src.gs) && stage(e.desc.fs, src.fs) && stage(e.desc.cs, src.cs);
    // Track timestamps even on failure so a fix triggers a rebuild.
    e.newest = {};
    for (auto& d : e.deps) {
        std::error_code ec;
        auto t = fs::last_write_time(d, ec);
        if (!ec && t > e.newest) e.newest = t;
    }
    if (ok) ok = device_->rebuildProgram(e.program.get(), src);
    if (!ok) {
        if (lastError_.empty()) lastError_ = e.program->log();
        std::fprintf(stderr, "[shaders] %s failed:\n%s\n", e.desc.name.c_str(), lastError_.c_str());
    }
    lastCompileMs_ = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
    return ok;
}

rhi::Program* ShaderLibrary::get(const rhi::ProgramDesc& desc) {
    std::string key = keyFor(desc, desc.defines);
    auto it = programs_.find(key);
    if (it != programs_.end()) return it->second.program.get();
    Entry e;
    e.desc = desc;
    if (e.desc.name.empty()) e.desc.name = !desc.cs.empty() ? desc.cs : desc.vs + "+" + desc.fs;
    e.program = device_->createProgram(e.desc);
    build(e);
    auto* p = e.program.get();
    programs_.emplace(key, std::move(e));
    return p;
}

rhi::Program* ShaderLibrary::graphics(const std::string& vs, const std::string& fsFile, std::vector<std::string> defines) {
    rhi::ProgramDesc d;
    d.vs = vs; d.fs = fsFile; d.defines = std::move(defines);
    return get(d);
}

rhi::Program* ShaderLibrary::compute(const std::string& cs, std::vector<std::string> defines) {
    rhi::ProgramDesc d;
    d.cs = cs; d.defines = std::move(defines);
    return get(d);
}

void ShaderLibrary::setGlobalDefine(const std::string& name, const std::string& value) {
    auto it = globalDefines_.find(name);
    if (it != globalDefines_.end() && it->second == value) return;
    globalDefines_[name] = value;
    reloadAll();
}

void ShaderLibrary::removeGlobalDefine(const std::string& name) {
    if (globalDefines_.erase(name)) reloadAll();
}

int ShaderLibrary::checkForChanges() {
    int rebuilt = 0;
    for (auto& [k, e] : programs_) {
        fs::file_time_type newest{};
        for (auto& d : e.deps) {
            std::error_code ec;
            auto t = fs::last_write_time(d, ec);
            if (!ec && t > newest) newest = t;
        }
        if (newest > e.newest) {
            build(e);
            ++rebuilt;
        }
    }
    if (rebuilt) ++reloadCount_;
    return rebuilt;
}

void ShaderLibrary::reloadAll() {
    for (auto& [k, e] : programs_) build(e);
    ++reloadCount_;
}

}  // namespace gfx
