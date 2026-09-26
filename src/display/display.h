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
};

struct OverlayConfig {
    float distanceM = 1.0f;   // how far in front of the eyes the panel sits
    float hfovDeg = 56.0f;    // camera horizontal field of view, for true-scale sizing
    float alpha = 0.85f;      // overlay opacity (lets passthrough show through)
    float offsetYM = 0.0f;    // vertical offset (camera is usually above eye level)
    bool dashboard = false;   // control panel in the SteamVR dashboard (opt-in until proven on the Frame)
    bool doubleBuffer = true;   // ping-pong between two overlays (single panel strobes on the Frame)
};

// Current settings, so a display can show them (e.g. on dashboard buttons).
struct UiState {
    std::string palette, gain;
    bool fahrenheit = false;
    bool operator==(const UiState& o) const {
        return palette == o.palette && gain == o.gain && fahrenheit == o.fahrenheit;
    }
};

class Display {
public:
    virtual ~Display() = default;
    virtual bool init(std::string& err) = 0;
    virtual void present(const thermal::RgbaImage& img) = 0;
    virtual DisplayEvents poll() = 0;
    virtual void setUiState(const UiState&) {}
    virtual const char* name() const = 0;
};

std::unique_ptr<Display> makeDesktopDisplay();                         // null if built without SDL2
std::unique_ptr<Display> makeOverlayDisplay(const OverlayConfig& cfg);  // null if built without OpenVR

// Adds (or removes) thermal-viewer as a SteamVR overlay application that
// starts automatically with SteamVR. Returns a process exit code.
int registerWithSteamVR(bool enable, const std::string& exeDir);
