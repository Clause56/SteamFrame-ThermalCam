// Where rendered frames go: a desktop window, a SteamVR overlay in the
// headset, or nowhere (headless snapshot mode).
#pragma once

#include <memory>
#include <string>

#include "../thermal/renderer.h"

struct DisplayEvents {
    bool quit = false;
    bool nextPalette = false;
    bool nextGain = false;
    bool snapshot = false;
    bool toggleUnits = false;
    bool toggleHud = false;
    bool rotate = false;           // turn the image a further 90 degrees clockwise
    bool settingsChanged = false;  // opacity/size/position changed (worth saving)
};

struct OverlayConfig {
    float distanceM = 1.0f;   // how far in front of the eyes the panel sits
    float hfovDeg = 56.0f;    // camera horizontal field of view, for true-scale sizing
    float alpha = 0.35f;      // overlay opacity (lets passthrough show through)
    float size = 0.95f;       // panel size relative to true scale
    float offsetXM = 0.0f;    // horizontal offset, + is right (lines the image up with passthrough)
    float offsetYM = 0.0f;    // vertical offset, + is up (camera is usually above eye level)
    bool dashboard = true;    // control panel in the SteamVR dashboard (confirmed working on the Frame)
    bool doubleBuffer = true;   // ping-pong between two overlays (single panel strobes on the Frame)
};

// Current settings, so a display can show them (e.g. on dashboard buttons).
struct UiState {
    std::string palette, gain;
    bool fahrenheit = false;
    int rotate = 0;  // degrees clockwise
    bool operator==(const UiState& o) const {
        return palette == o.palette && gain == o.gain && fahrenheit == o.fahrenheit && rotate == o.rotate;
    }
};

class Display {
public:
    virtual ~Display() = default;
    virtual bool init(std::string& err) = 0;
    virtual void present(const thermal::RgbaImage& img) = 0;
    virtual DisplayEvents poll() = 0;
    virtual void setUiState(const UiState&) {}
    // The overlay's current opacity/size/position, if this display has them.
    virtual bool overlaySettings(OverlayConfig&) const { return false; }
    virtual const char* name() const = 0;
};

std::unique_ptr<Display> makeDesktopDisplay();                         // null if built without SDL2
std::unique_ptr<Display> makeOverlayDisplay(const OverlayConfig& cfg);  // null if built without OpenVR

// Adds (or removes) thermal-viewer as a SteamVR overlay application that
// starts automatically with SteamVR. Returns a process exit code.
int registerWithSteamVR(bool enable, const std::string& exeDir);
