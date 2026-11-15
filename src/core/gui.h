// CoreGui — Dear ImGui settings menu (generated from SettingsRegistry), doc viewer and performance overlay.
#pragma once

#include <map>
#include <string>

namespace gfx {

class Engine;
struct FrameContext;

class CoreGui {
public:
    explicit CoreGui(Engine& e) : engine_(e) {}
    ~CoreGui();
    void init();
    // full = real frame (menu + overlay + module windows); false = generated frame (overlay only)
    void render(FrameContext& ctx, bool full);
    bool wantsKeyboard() const;
    bool wantsMouse() const;
    void openDoc(const std::string& doc);

    bool showMenu = true;
    bool showOverlay = true;
    bool showDebugSettings = false;

private:
    void drawMenu(FrameContext& ctx);
    void drawOverlay(FrameContext& ctx);
    void drawDoc();
    Engine& engine_;
    bool initialized_ = false;
    std::string docName_, docText_;
    bool docOpen_ = false;
    std::map<std::string, float> dragging_;
};

}  // namespace gfx
