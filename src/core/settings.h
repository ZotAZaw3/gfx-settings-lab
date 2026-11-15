// SettingsRegistry — every graphics option in the app lives here.
// Modules register settings in Module::registerSettings(); the menu (src/core/settings_menu.cpp)
// is generated from this registry: one tab per category, grouped, with per-preset values,
// tooltips, a "?" link to docs/<doc>.md, and the measured cost of each setting.
#pragma once

#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gfx {

enum class SettingType { Bool, Int, Float, Enum };

// Quality presets, like a game's "Graphics Quality" dropdown.
enum class Preset : int { Low = 0, Medium = 1, High = 2, Ultra = 3, Custom = 4 };
inline constexpr int kPresetCount = 4;

enum SettingFlags : unsigned {
    SettingNone            = 0,
    SettingRequiresResize  = 1u << 0,   // render targets are rebuilt (Module::resize is called)
    SettingRequiresReload  = 1u << 1,   // shaders recompiled (permutation define)
    SettingRequiresRestart = 1u << 2,   // takes effect on next launch
    SettingNoPreset        = 1u << 3,   // presets never touch it (e.g. resolution, vsync, gamma)
    SettingDebug           = 1u << 4,   // shown only with "Show debug settings"
};

struct SettingDesc {
    std::string id;         // unique, dotted: "<module>.<name>", e.g. "post.bloom.intensity"
    std::string label;      // shown in the menu
    std::string category;   // menu tab: Display, Graphics, Textures, Lighting, Shadows, Ray Tracing,
                            // Anti-Aliasing & Upscaling, Geometry & VFX, Post Processing, Latency, Debug
    std::string group;      // optional sub-header inside the tab
    std::string tooltip;    // one-line definition shown on hover
    std::string doc;        // docs/<doc>.md stem, e.g. "bloom"
    unsigned    flags = SettingNone;
};

class Setting {
public:
    SettingDesc desc;
    SettingType type;

    // Current value storage (all types share these; read the typed accessor you registered).
    bool  b = false;
    int   i = 0;
    float f = 0.0f;

    // Range / options
    int   imin = 0, imax = 1;
    float fmin = 0.0f, fmax = 1.0f;
    const char* format = "%.2f";
    std::vector<std::string> options;   // Enum labels

    // Default and per-preset values (stored as float; converted by type)
    float defaultValue = 0.0f;
    bool  hasPresets = false;
    float presetValues[kPresetCount] = {0, 0, 0, 0};

    std::function<bool()> visibleIf;    // hide when false (e.g. TAA options when TAA is off)
    std::function<bool()> enabledIf;    // grey out when false (e.g. unsupported on this GPU)
    std::vector<std::function<void(Setting&)>> onChange;

    // ---- fluent configuration ----
    Setting& presets(float low, float medium, float high, float ultra);
    Setting& visible(std::function<bool()> fn) { visibleIf = std::move(fn); return *this; }
    Setting& enabled(std::function<bool()> fn) { enabledIf = std::move(fn); return *this; }
    Setting& changed(std::function<void(Setting&)> fn) { onChange.push_back(std::move(fn)); return *this; }
    Setting& fmt(const char* f) { format = f; return *this; }

    // ---- typed access ----
    bool  asBool() const { return b; }
    int   asInt() const { return i; }     // Int and Enum
    float asFloat() const { return f; }
    float asNumber() const;               // any type as float (for profiling/benchmarks)
    void  setNumber(float v);             // raw set, clamps, NO callbacks (use SettingsRegistry::set)
    std::string valueString() const;
};

class SettingsRegistry {
public:
    Setting& addBool (SettingDesc d, bool def);
    Setting& addInt  (SettingDesc d, int def, int min, int max);
    Setting& addFloat(SettingDesc d, float def, float min, float max);
    Setting& addEnum (SettingDesc d, std::vector<std::string> options, int def);

    Setting*       find(const std::string& id);
    const Setting* find(const std::string& id) const;
    bool  getBool(const std::string& id) const;
    int   getInt(const std::string& id) const;
    float getFloat(const std::string& id) const;

    // Change a setting from code (other modules, calibration screens): clamps, fires callbacks + profiler.
    bool   set(const std::string& id, float value);

    void   applyPreset(Preset p);
    Preset currentPreset() const { return preset_; }
    void   markCustom() { preset_ = Preset::Custom; }

    bool load(const std::string& path);   // simple "id=value" ini
    bool save(const std::string& path) const;

    std::deque<Setting>& all() { return settings_; }
    const std::deque<Setting>& all() const { return settings_; }
    std::vector<std::string> categories() const;   // ordered for the menu tabs

    // Set by the menu when a value changes; Engine consumes these each frame.
    bool consumeResizeRequest()  { bool r = resizeRequested_;  resizeRequested_ = false; return r; }
    bool consumeReloadRequest()  { bool r = reloadRequested_;  reloadRequested_ = false; return r; }
    // Call after changing a value: runs callbacks, flags resize/reload, informs the profiler.
    // `oldValue` (asNumber() before the change) lets the profiler label the cost probe.
    void notifyChanged(Setting& s, float oldValue = 0.0f);
    std::function<void(Setting&, float oldValue)> onAnyChange;   // Engine -> profiler

private:
    Setting& add(SettingDesc d, SettingType t);
    std::deque<Setting> settings_;    // deque: references stay valid as modules add settings
    std::unordered_map<std::string, size_t> index_;
    Preset preset_ = Preset::High;
    bool resizeRequested_ = false, reloadRequested_ = false;
};

}  // namespace gfx
