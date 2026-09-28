// Turns a ThermalFrame into an RGBA image: false colour, automatic gain,
// crosshair, hot/cold markers, temperature readouts and a colour scale.
// The output is a plain RGBA buffer so any display (desktop window, VR
// overlay texture, PNG snapshot) can show it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "thermal_frame.h"

namespace thermal {

struct RgbaImage {
    int width = 0, height = 0;
    std::vector<uint8_t> px;  // RGBA8, row-major
    // Where the thermal image sits inside the frame (the rest is HUD).
    int imageX = 0, imageY = 0, imageW = 0, imageH = 0;
};

enum class Palette { Ironbow, WhiteHot, BlackHot, Rainbow, Arctic, Count };

const char* paletteName(Palette p);
bool parsePalette(const std::string& s, Palette& out);

// How temperatures become colours.
//  Equalize: histogram equalization + detail boost on the temperature data.
//  Camera:   colour the camera's own processed 8-bit picture (the top half of
//            the TC001 stream); temperatures still come from the data.
//  Linear:   straight min..max mapping (best for judging absolute heat).
enum class GainMode { Equalize, Camera, Linear, Count };

const char* gainModeName(GainMode g);
bool parseGainMode(const std::string& s, GainMode& out);

struct RenderOptions {
    Palette palette = Palette::Arctic;
    GainMode gain = GainMode::Equalize;
    float detail = 0.5f;         // detail boost for Equalize, 0..2
    int scale = 3;               // output = sensor size * scale (bilinear)
    bool fixedRange = false;     // use lo/hi instead of automatic gain
    float lo = 20.f, hi = 40.f;  // fixed range, in frame units (°C when radiometric)
    bool crosshair = true;
    bool markers = true;         // hottest / coldest spot markers
    bool stats = true;           // text readout
    bool colorBar = true;
    bool fahrenheit = false;
};

struct FrameStats {
    float min = 0, max = 0, center = 0;
    int minX = 0, minY = 0, maxX = 0, maxY = 0;
};

FrameStats computeStats(const ThermalFrame& f);

class Renderer {
public:
    void render(const ThermalFrame& f, const RenderOptions& opt, RgbaImage& out);
    const FrameStats& lastStats() const { return stats_; }
    float gainLo() const { return lo_; }
    float gainHi() const { return hi_; }
    GainMode effectiveGain() const { return effGain_; }

private:
    void updateGain(const ThermalFrame& f, const RenderOptions& opt);
    void mapToUnit(const ThermalFrame& f, const RenderOptions& opt);
    void equalize(const ThermalFrame& f, const RenderOptions& opt);

    FrameStats stats_;
    float lo_ = 0, hi_ = 1;
    bool gainInit_ = false;
    std::vector<float> scratch_;
    std::vector<float> unit_;  // per-sensor-pixel colour position, 0..1
    std::vector<float> cdf_;   // smoothed equalization curve
    GainMode effGain_ = GainMode::Linear;
};

// Low-level drawing helpers, also used by the displays for status text.
void fillRect(RgbaImage& img, int x, int y, int w, int h, uint32_t rgba);
void drawText(RgbaImage& img, int x, int y, const std::string& text, int scale, uint32_t rgba);
int textWidth(const std::string& text, int scale);

}  // namespace thermal
