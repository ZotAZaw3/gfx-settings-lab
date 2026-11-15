#include "core/profiler.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "core/settings.h"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace gfx {

using Clock = std::chrono::high_resolution_clock;
static double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

static constexpr uint32_t kTimerRing = 6;   // results read ~5 frames later, never stalls

void Profiler::init(rhi::Device& device) {
    device_ = &device;
    lastFrameEnd_ = Clock::now();
    lastCpuSample_ = Clock::now();
}

double Profiler::processRamMB() const {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return pmc.WorkingSetSize / (1024.0 * 1024.0);
    return 0.0;
#else
    long rssPages = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {
        long dummy = 0;
        if (std::fscanf(f, "%ld %ld", &dummy, &rssPages) != 2) rssPages = 0;
        std::fclose(f);
    }
    return double(rssPages) * double(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
#endif
}

static double processCpuSeconds() {
#if defined(_WIN32)
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0.0;
    auto toSec = [](FILETIME f) { return (double(f.dwHighDateTime) * 4294967296.0 + f.dwLowDateTime) * 1e-7; };
    return toSec(k) + toSec(u);
#else
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
    return r.ru_utime.tv_sec + r.ru_utime.tv_usec * 1e-6 + r.ru_stime.tv_sec + r.ru_stime.tv_usec * 1e-6;
#endif
}

rhi::MemoryStats Profiler::memory() const { return device_->memoryStats(); }

void Profiler::beginFrame() {
    frameStart_ = Clock::now();
    frameGpuAccum_ = 0.0;
}

Profiler::PassTimer* Profiler::beginPass(const std::string& key) {
    PassData& d = passes_[key];
    if (d.timers.empty() && device_->caps().timestampQueries)
        for (uint32_t i = 0; i < kTimerRing; ++i) d.timers.push_back(device_->createTimer());
    rhi::TimerQuery* q = nullptr;
    if (!d.timers.empty()) {
        // Harvest the oldest result in the ring before reusing its slot.
        rhi::TimerQuery* slot = d.timers[d.ringIndex].get();
        if (slot->resultReady()) {
            d.lastGpu = slot->resultMs();
            d.gpuMs = d.gpuMs * 0.9 + d.lastGpu * 0.1;
        }
        q = slot;
        d.ringIndex = (d.ringIndex + 1) % uint32_t(d.timers.size());
        device_->cmd().beginTimer(q);
    }
    d.cpuStart = Clock::now();
    live_.push_back({&d, q});
    return &live_.back();
}

void Profiler::endPass(PassTimer* t) {
    if (!t) return;
    PassTimer pt = *t;
    if (pt.q) device_->cmd().endTimer(pt.q);
    double cpu = msSince(pt.data->cpuStart);
    pt.data->cpuMs = pt.data->cpuMs * 0.9 + cpu * 0.1;
    frameGpuAccum_ += pt.data->lastGpu;
    live_.pop_back();
}

double Profiler::passGpuMs(const std::string& key) const { auto it = passes_.find(key); return it == passes_.end() ? 0.0 : it->second.gpuMs; }
double Profiler::passCpuMs(const std::string& key) const { auto it = passes_.find(key); return it == passes_.end() ? 0.0 : it->second.cpuMs; }

std::vector<Profiler::PassStat> Profiler::passStats() const {
    std::vector<PassStat> out;
    for (auto& [k, d] : passes_) out.push_back({k, d.gpuMs, d.cpuMs});
    return out;
}

void Profiler::endFrame(double waitMs) {
    auto now = Clock::now();
    FrameSample s;
    s.frameMs = std::chrono::duration<double, std::milli>(now - lastFrameEnd_).count();
    s.cpuMs = std::max(0.0, std::chrono::duration<double, std::milli>(now - frameStart_).count() - waitMs);
    s.gpuMs = frameGpuAccum_;
    s.waitMs = waitMs;
    s.ramMB = processRamMB();
    auto mem = device_->memoryStats();
    s.vramMB = double(mem.textureBytes + mem.bufferBytes) / (1024.0 * 1024.0);
    lastFrameEnd_ = now;
    history_.push_back(s);
    while (history_.size() > 600) history_.pop_front();

    double sinceCpu = std::chrono::duration<double>(now - lastCpuSample_).count();
    if (sinceCpu > 0.5) {
        double cpuSec = processCpuSeconds();
        cpuPercent_ = 100.0 * (cpuSec - lastProcCpuSec_) / sinceCpu;
        lastProcCpuSec_ = cpuSec;
        lastCpuSample_ = now;
    }

    if (probe_.active) {
        ++probe_.frames;
        if (probe_.frames > kWarmup) probe_.after.push_back(s);
        if (int(probe_.after.size()) >= kWindow) {
            FrameSample a{};
            for (auto& f : probe_.after) { a.frameMs += f.frameMs; a.cpuMs += f.cpuMs; a.gpuMs += f.gpuMs; a.ramMB += f.ramMB; a.vramMB += f.vramMB; }
            double n = double(probe_.after.size());
            SettingCost c;
            c.from = probe_.from; c.to = probe_.to; c.framesMeasured = int(n);
            c.delta.frameMs = a.frameMs / n - probe_.before.frameMs;
            c.delta.cpuMs = a.cpuMs / n - probe_.before.cpuMs;
            c.delta.gpuMs = a.gpuMs / n - probe_.before.gpuMs;
            c.delta.ramMB = a.ramMB / n - probe_.before.ramMB;
            c.delta.vramMB = a.vramMB / n - probe_.before.vramMB;
            c.delta.valid = true;
            costs_[probe_.id] = c;
            lastProbeResult_ = c.delta;
            probe_ = Probe{};
        }
    }
}

FrameSample Profiler::average(int frames) const {
    FrameSample a{};
    int n = std::min<int>(frames, int(history_.size()));
    if (n == 0) return a;
    for (int i = int(history_.size()) - n; i < int(history_.size()); ++i) {
        const auto& f = history_[i];
        a.frameMs += f.frameMs; a.cpuMs += f.cpuMs; a.gpuMs += f.gpuMs; a.waitMs += f.waitMs; a.ramMB += f.ramMB; a.vramMB += f.vramMB;
    }
    a.frameMs /= n; a.cpuMs /= n; a.gpuMs /= n; a.waitMs /= n; a.ramMB /= n; a.vramMB /= n;
    return a;
}

double Profiler::percentileFrameMs(double pct, int frames) const {
    int n = std::min<int>(frames, int(history_.size()));
    if (n == 0) return 0.0;
    std::vector<double> v;
    for (int i = int(history_.size()) - n; i < int(history_.size()); ++i) v.push_back(history_[i].frameMs);
    std::sort(v.begin(), v.end());
    size_t idx = std::min(v.size() - 1, size_t(pct / 100.0 * double(v.size())));
    return v[idx];
}

double Profiler::gpuBusyPercent() const {
    FrameSample a = average(30);
    return a.frameMs > 0.0 ? std::min(100.0, 100.0 * a.gpuMs / a.frameMs) : 0.0;
}

void Profiler::onSettingChanged(Setting& s, float oldValue) {
    // Restart the probe: baseline = rolling window before this change.
    probe_ = Probe{};
    probe_.active = true;
    probe_.id = s.desc.id;
    Setting tmp = s;
    tmp.setNumber(oldValue);
    probe_.from = tmp.valueString();
    probe_.to = s.valueString();
    probe_.before = average(kWindow);
}

const SettingCost* Profiler::costOf(const std::string& id) const {
    auto it = costs_.find(id);
    return it == costs_.end() ? nullptr : &it->second;
}

void Profiler::writeSweepReport(const std::string& dir, const rhi::Caps& caps) const {
    std::filesystem::create_directories(dir);
    std::string gpu = caps.renderer;
    for (char& c : gpu) if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    std::ofstream csv(dir + "/" + gpu + ".csv");
    csv << "setting,label,category,value,d_gpu_ms,d_cpu_ms,d_frame_ms,d_vram_mb,d_ram_mb\n";
    for (auto& r : sweepResults)
        csv << r.settingId << ",\"" << r.label << "\",\"" << r.category << "\",\"" << r.value << "\"," << r.cost.gpuMs << ","
            << r.cost.cpuMs << "," << r.cost.frameMs << "," << r.cost.vramMB << "," << r.cost.ramMB << "\n";
    std::ofstream md(dir + "/" + gpu + ".md");
    md << "# Per-setting cost: " << caps.renderer << "\n\n" << caps.apiName << " " << caps.version
       << ". Deltas vs the High preset baseline, averaged over " << kWindow << " frames after " << kWarmup
       << " warm-up frames. Generated by `gfxlab --bench-settings`.\n\n";
    std::string cat;
    for (auto& r : sweepResults) {
        if (r.category != cat) {
            cat = r.category;
            md << "\n## " << cat << "\n\n| Setting | Value | Δ GPU ms | Δ CPU ms | Δ frame ms | Δ VRAM MB | Δ RAM MB |\n|---|---|---|---|---|---|---|\n";
        }
        char line[512];
        std::snprintf(line, sizeof(line), "| %s | %s | %+.3f | %+.3f | %+.3f | %+.1f | %+.1f |\n", r.label.c_str(), r.value.c_str(),
                      r.cost.gpuMs, r.cost.cpuMs, r.cost.frameMs, r.cost.vramMB, r.cost.ramMB);
        md << line;
    }
}

CpuScope::CpuScope(Profiler& p_, const char* n) : p(p_), name(n), start(Clock::now()) {}
CpuScope::~CpuScope() { p.counter(std::string("cpu ms: ") + name, msSince(start)); }

}  // namespace gfx
