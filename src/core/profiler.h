// Profiler — CPU / GPU / RAM / VRAM cost of every pass, module and setting.
//
// Automatic (modules do nothing):
//   * every Pass gets a GPU timestamp pair + CPU timer (shown per pass in the overlay)
//   * every rhi resource is tagged with its owner -> VRAM per module
//   * when a setting changes, the profiler compares a rolling window before the change with a
//     window after it (warm-up frames skipped) and stores the delta on that setting:
//     GPU ms, CPU ms, frame ms, VRAM MB, RAM MB. The settings menu shows it next to the control.
//   * --bench-settings runs that measurement for every value of every setting and writes
//     docs/_measurements/<gpu>.md + .csv, which docs/<setting>.md files quote.
// Optional (modules):
//   * GFX_CPU_SCOPE(profiler, "BVH build") for sub-pass CPU timing
//   * profiler.counter("particles.alive", n) for custom stats
//   * Module::cpuMemoryBytes() for CPU-side memory
#pragma once

#include <chrono>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "rhi/rhi.h"
#include "core/module.h"   // GFX_CONCAT

namespace gfx {

class Setting;
class SettingsRegistry;

struct FrameSample {
    double frameMs = 0.0;     // wall time between presents
    double cpuMs = 0.0;       // CPU time spent building the frame (excl. waits) + modules' off-thread CPU
    double gpuMs = 0.0;       // sum of pass GPU timings
    double waitMs = 0.0;      // time blocked in latency limiter / vsync
    double ramMB = 0.0;       // process resident set
    double vramMB = 0.0;      // tracked rhi allocations
};

struct ResourceCost {
    double gpuMs = 0.0, cpuMs = 0.0, frameMs = 0.0, vramMB = 0.0, ramMB = 0.0;
    bool   valid = false;
};

struct SettingCost {        // measured delta: (after change) - (before change)
    ResourceCost delta;
    std::string  from, to;  // value strings
    int          framesMeasured = 0;
};

class Profiler {
public:
    void init(rhi::Device& device);

    // Frame boundaries (Engine)
    void beginFrame();
    void endFrame(double waitMs, double offThreadCpuMs = 0.0);

    // Pass timing (Engine wraps every pass)
    struct PassTimer;
    PassTimer* beginPass(const std::string& key);
    void endPass(PassTimer* t);
    double passGpuMs(const std::string& key) const;
    double passCpuMs(const std::string& key) const;

    // Custom stats
    void counter(const std::string& name, double value) { counters_[name] = value; }
    const std::map<std::string, double>& counters() const { return counters_; }

    // Setting cost probes (SettingsRegistry::notifyChanged -> onSettingChanged)
    void onSettingChanged(Setting& s, float oldValue);
    const SettingCost* costOf(const std::string& settingId) const;
    bool measuring() const { return probe_.active; }
    const std::string& measuringId() const { return probe_.id; }
    ResourceCost lastProbeResult_;

    // Rolling stats
    const std::deque<FrameSample>& history() const { return history_; }
    FrameSample average(int frames) const;
    double percentileFrameMs(double pct, int frames) const;   // e.g. 99 -> "1% low" frame time
    double processRamMB() const;
    double processCpuPercent() const { return cpuPercent_; }  // CPU usage of this process (all cores = 100*n)
    double gpuBusyPercent() const;                            // gpuMs / frameMs
    rhi::MemoryStats memory() const;

    struct PassStat { std::string key; double gpuMs, cpuMs; };
    std::vector<PassStat> passStats() const;

    // Benchmark sweep (Engine drives frames; profiler keeps the results)
    struct SweepResult { std::string settingId, label, category, value; ResourceCost cost; };
    std::vector<SweepResult> sweepResults;
    void writeSweepReport(const std::string& dir, const rhi::Caps& caps) const;

    static constexpr int kWindow = 60;       // frames per measurement window
    static constexpr int kWarmup = 15;       // frames ignored after a change (shader compile, RT realloc)

private:
    struct PassData {
        std::vector<rhi::TimerPtr> timers;   // ring, results read N frames later
        double gpuMs = 0.0, cpuMs = 0.0;     // smoothed
        double lastGpu = 0.0;
        std::chrono::high_resolution_clock::time_point cpuStart;
        uint32_t ringIndex = 0;
    };
public:
    struct PassTimer { PassData* data; rhi::TimerQuery* q; };
private:
    rhi::Device* device_ = nullptr;
    std::map<std::string, PassData> passes_;
    std::vector<PassTimer> live_;
    std::map<std::string, double> counters_;
    std::deque<FrameSample> history_;
    std::map<std::string, SettingCost> costs_;
    std::chrono::high_resolution_clock::time_point frameStart_, lastFrameEnd_;
    double frameGpuAccum_ = 0.0;
    double cpuPercent_ = 0.0;
    double lastProcCpuSec_ = 0.0;
    std::chrono::high_resolution_clock::time_point lastCpuSample_;

    struct Probe {
        bool active = false;
        std::string id, from, to;
        FrameSample before;
        int frames = 0;
        std::vector<FrameSample> after;
    } probe_;
};

struct CpuScope {
    CpuScope(Profiler& p, const char* name);
    ~CpuScope();
    Profiler& p; const char* name;
    std::chrono::high_resolution_clock::time_point start;
};
#define GFX_CPU_SCOPE(profiler, name) ::gfx::CpuScope GFX_CONCAT(_gfxCpuScope, __LINE__)(profiler, name)

}  // namespace gfx
