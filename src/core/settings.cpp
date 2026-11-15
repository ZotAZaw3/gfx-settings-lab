#include "core/settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace gfx {

Setting& Setting::presets(float low, float medium, float high, float ultra) {
    hasPresets = true;
    presetValues[0] = low; presetValues[1] = medium; presetValues[2] = high; presetValues[3] = ultra;
    return *this;
}

float Setting::asNumber() const {
    switch (type) {
        case SettingType::Bool: return b ? 1.0f : 0.0f;
        case SettingType::Int: case SettingType::Enum: return float(i);
        default: return f;
    }
}

std::string Setting::valueString() const {
    char buf[64];
    switch (type) {
        case SettingType::Bool: return b ? "On" : "Off";
        case SettingType::Int: std::snprintf(buf, sizeof(buf), "%d", i); return buf;
        case SettingType::Enum: return (i >= 0 && i < int(options.size())) ? options[i] : std::to_string(i);
        default: std::snprintf(buf, sizeof(buf), format, f); return buf;
    }
}

// Note: setNumber does NOT fire callbacks by itself; SettingsRegistry::notifyChanged does.
void Setting::setNumber(float v) {
    switch (type) {
        case SettingType::Bool: b = v >= 0.5f; break;
        case SettingType::Int: i = std::clamp(int(std::lround(v)), imin, imax); break;
        case SettingType::Enum: i = std::clamp(int(std::lround(v)), 0, std::max(0, int(options.size()) - 1)); break;
        default: f = std::clamp(v, fmin, fmax); break;
    }
}

Setting& SettingsRegistry::add(SettingDesc d, SettingType t) {
    auto it = index_.find(d.id);
    if (it != index_.end()) {
        std::fprintf(stderr, "[settings] duplicate id '%s' (keeping the first registration)\n", d.id.c_str());
        return settings_[it->second];
    }
    settings_.emplace_back();
    Setting& s = settings_.back();
    s.desc = std::move(d);
    s.type = t;
    if (s.desc.category.empty()) s.desc.category = "Graphics";
    index_[s.desc.id] = settings_.size() - 1;
    return s;
}

Setting& SettingsRegistry::addBool(SettingDesc d, bool def) {
    Setting& s = add(std::move(d), SettingType::Bool);
    s.b = def; s.defaultValue = def ? 1.0f : 0.0f;
    return s;
}
Setting& SettingsRegistry::addInt(SettingDesc d, int def, int min, int max) {
    Setting& s = add(std::move(d), SettingType::Int);
    s.imin = min; s.imax = max; s.i = std::clamp(def, min, max); s.defaultValue = float(s.i);
    return s;
}
Setting& SettingsRegistry::addFloat(SettingDesc d, float def, float min, float max) {
    Setting& s = add(std::move(d), SettingType::Float);
    s.fmin = min; s.fmax = max; s.f = std::clamp(def, min, max); s.defaultValue = s.f;
    return s;
}
Setting& SettingsRegistry::addEnum(SettingDesc d, std::vector<std::string> options, int def) {
    Setting& s = add(std::move(d), SettingType::Enum);
    s.options = std::move(options);
    s.imin = 0; s.imax = int(s.options.size()) - 1;
    s.i = std::clamp(def, 0, std::max(0, s.imax)); s.defaultValue = float(s.i);
    return s;
}

Setting* SettingsRegistry::find(const std::string& id) {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : &settings_[it->second];
}
const Setting* SettingsRegistry::find(const std::string& id) const {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : &settings_[it->second];
}
bool SettingsRegistry::getBool(const std::string& id) const { auto* s = find(id); return s && s->b; }
int SettingsRegistry::getInt(const std::string& id) const { auto* s = find(id); return s ? s->i : 0; }
float SettingsRegistry::getFloat(const std::string& id) const { auto* s = find(id); return s ? s->f : 0.0f; }

void SettingsRegistry::notifyChanged(Setting& s, float oldValue) {
    for (auto& fn : s.onChange) fn(s);
    if (s.desc.flags & SettingRequiresResize) resizeRequested_ = true;
    if (s.desc.flags & SettingRequiresReload) reloadRequested_ = true;
    if (onAnyChange) onAnyChange(s, oldValue);
}

bool SettingsRegistry::set(const std::string& id, float value) {
    Setting* s = find(id);
    if (!s) return false;
    float before = s->asNumber();
    s->setNumber(value);
    if (s->asNumber() != before) notifyChanged(*s, before);
    return true;
}

void SettingsRegistry::applyPreset(Preset p) {
    if (p == Preset::Custom) { preset_ = p; return; }
    int idx = int(p);
    for (auto& s : settings_) {
        if (!s.hasPresets || (s.desc.flags & SettingNoPreset)) continue;
        float before = s.asNumber();
        s.setNumber(s.presetValues[idx]);
        if (s.asNumber() != before) notifyChanged(s, before);
    }
    preset_ = p;
}

std::vector<std::string> SettingsRegistry::categories() const {
    static const char* order[] = {"Display", "Graphics", "Textures", "Lighting", "Shadows", "Ray Tracing",
                                  "Anti-Aliasing & Upscaling", "Geometry", "World", "Effects", "Characters",
                                  "Physics", "Post Processing", "Latency", "Performance", "Audio", "Debug"};
    std::vector<std::string> out;
    for (const char* c : order)
        for (auto& s : settings_)
            if (s.desc.category == c) { out.push_back(c); break; }
    for (auto& s : settings_)
        if (std::find(out.begin(), out.end(), s.desc.category) == out.end()) out.push_back(s.desc.category);
    return out;
}

bool SettingsRegistry::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        if (key == "preset") { preset_ = Preset(std::clamp(std::atoi(val.c_str()), 0, 4)); continue; }
        if (Setting* s = find(key)) {
            float before = s->asNumber();
            s->setNumber(std::strtof(val.c_str(), nullptr));
            if (s->asNumber() != before) notifyChanged(*s, before);
        }
    }
    return true;
}

bool SettingsRegistry::save(const std::string& path) const {
    std::ofstream out(path);
    if (!out) return false;
    out << "# gfx-settings-lab settings\npreset=" << int(preset_) << "\n";
    for (auto& s : settings_) out << s.desc.id << "=" << s.asNumber() << "\n";
    return true;
}

}  // namespace gfx
