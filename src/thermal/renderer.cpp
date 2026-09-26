#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "font5x7.h"

namespace thermal {
namespace {

struct Stop {
    float t;
    uint8_t r, g, b;
};

std::vector<uint32_t> buildLut(const std::vector<Stop>& stops) {
    std::vector<uint32_t> lut(256);
    for (int i = 0; i < 256; ++i) {
        float t = i / 255.f;
        size_t k = 1;
        while (k < stops.size() - 1 && stops[k].t < t) ++k;
        const Stop& a = stops[k - 1];
        const Stop& b = stops[k];
        float u = b.t > a.t ? std::clamp((t - a.t) / (b.t - a.t), 0.f, 1.f) : 0.f;
        auto mix = [u](uint8_t x, uint8_t y) { return uint32_t(std::lround(x + (y - x) * u)); };
        lut[i] = mix(a.r, b.r) | (mix(a.g, b.g) << 8) | (mix(a.b, b.b) << 16) | 0xff000000u;
    }
    return lut;
}

const std::vector<uint32_t>& lutFor(Palette p) {
    static const std::vector<uint32_t> luts[] = {
        buildLut({{0, 0, 0, 0}, {.15f, 30, 0, 110}, {.35f, 140, 0, 150}, {.55f, 220, 60, 40},
                  {.75f, 250, 160, 0}, {.9f, 255, 225, 70}, {1, 255, 255, 255}}),  // Ironbow
        buildLut({{0, 0, 0, 0}, {1, 255, 255, 255}}),                              // White hot
        buildLut({{0, 255, 255, 255}, {1, 0, 0, 0}}),                              // Black hot
        buildLut({{0, 20, 0, 120}, {.25f, 0, 110, 255}, {.5f, 0, 230, 120}, {.75f, 255, 230, 0},
                  {1, 255, 20, 0}}),                                               // Rainbow
        buildLut({{0, 10, 20, 70}, {.4f, 20, 90, 200}, {.7f, 120, 200, 255}, {.85f, 255, 200, 60},
                  {1, 255, 255, 255}}),                                            // Arctic
    };
    return luts[int(p)];
}

inline void put(RgbaImage& img, int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= img.width || y >= img.height) return;
    uint8_t* p = &img.px[(size_t(y) * img.width + x) * 4];
    uint8_t a = c >> 24;
    if (a == 255) {
        p[0] = c & 0xff; p[1] = (c >> 8) & 0xff; p[2] = (c >> 16) & 0xff; p[3] = 255;
        return;
    }
    for (int i = 0; i < 3; ++i) p[i] = uint8_t((p[i] * (255 - a) + ((c >> (8 * i)) & 0xff) * a) / 255);
}

constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24);
}

std::string fmtValue(float v, Units u, bool fahrenheit) {
    char s[32];
    if (u == Units::Celsius) {
        if (fahrenheit) snprintf(s, sizeof s, "%.1f*F", v * 9.f / 5.f + 32.f);
        else snprintf(s, sizeof s, "%.1f*C", v);
    } else {
        snprintf(s, sizeof s, "%.0f", v);
    }
    return s;
}

void drawCross(RgbaImage& img, int cx, int cy, int r, int t, uint32_t c) {
    fillRect(img, cx - r, cy - t / 2, r * 2 + 1, t, c);
    fillRect(img, cx - t / 2, cy - r, t, r * 2 + 1, c);
}

void drawBox(RgbaImage& img, int cx, int cy, int r, int t, uint32_t c) {
    fillRect(img, cx - r, cy - r, 2 * r + 1, t, c);
    fillRect(img, cx - r, cy + r - t + 1, 2 * r + 1, t, c);
    fillRect(img, cx - r, cy - r, t, 2 * r + 1, c);
    fillRect(img, cx + r - t + 1, cy - r, t, 2 * r + 1, c);
}

}  // namespace

const char* paletteName(Palette p) {
    switch (p) {
        case Palette::Ironbow: return "ironbow";
        case Palette::WhiteHot: return "white";
        case Palette::BlackHot: return "black";
        case Palette::Rainbow: return "rainbow";
        case Palette::Arctic: return "arctic";
        default: return "?";
    }
}

const char* gainModeName(GainMode g) {
    switch (g) {
        case GainMode::Equalize: return "equalize";
        case GainMode::Camera: return "camera";
        case GainMode::Linear: return "linear";
        default: return "?";
    }
}

bool parseGainMode(const std::string& s, GainMode& out) {
    for (int i = 0; i < int(GainMode::Count); ++i)
        if (s == gainModeName(GainMode(i))) {
            out = GainMode(i);
            return true;
        }
    return false;
}

bool parsePalette(const std::string& s, Palette& out) {
    for (int i = 0; i < int(Palette::Count); ++i)
        if (s == paletteName(Palette(i))) {
            out = Palette(i);
            return true;
        }
    return false;
}

void fillRect(RgbaImage& img, int x, int y, int w, int h, uint32_t c) {
    int x0 = std::max(0, x), y0 = std::max(0, y);
    int x1 = std::min(img.width, x + w), y1 = std::min(img.height, y + h);
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) put(img, xx, yy, c);
}

int textWidth(const std::string& text, int scale) { return int(text.size()) * 6 * scale - (text.empty() ? 0 : scale); }

void drawText(RgbaImage& img, int x, int y, const std::string& text, int sc, uint32_t c) {
    for (char ch : text) {
        const uint8_t* g = glyph5x7(ch);
        if (g)
            for (int r = 0; r < 7; ++r)
                for (int col = 0; col < 5; ++col)
                    if (g[r] & (0x10 >> col)) fillRect(img, x + col * sc, y + r * sc, sc, sc, c);
        x += 6 * sc;
    }
}

FrameStats computeStats(const ThermalFrame& f) {
    FrameStats s;
    if (f.value.empty()) return s;
    s.min = s.max = f.value[0];
    for (int y = 0; y < f.height; ++y)
        for (int x = 0; x < f.width; ++x) {
            float v = f.value[size_t(y) * f.width + x];
            if (v < s.min) { s.min = v; s.minX = x; s.minY = y; }
            if (v > s.max) { s.max = v; s.maxX = x; s.maxY = y; }
        }
    // Centre spot: mean of the 3x3 block, less noisy than one pixel.
    float sum = 0;
    int n = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            int x = f.width / 2 + dx, y = f.height / 2 + dy;
            if (x >= 0 && y >= 0 && x < f.width && y < f.height) { sum += f.value[size_t(y) * f.width + x]; ++n; }
        }
    s.center = n ? sum / n : 0;
    return s;
}

void Renderer::updateGain(const ThermalFrame& f, const RenderOptions& opt) {
    if (opt.fixedRange) {
        lo_ = opt.lo;
        hi_ = opt.hi;
        gainInit_ = false;
        return;
    }
    // 1st/99th percentile so a single hot pixel doesn't wash out the image,
    // smoothed over time so the palette doesn't flicker.
    scratch_ = f.value;
    size_t n = scratch_.size();
    size_t a = n / 100, b = n - 1 - n / 100;
    std::nth_element(scratch_.begin(), scratch_.begin() + a, scratch_.end());
    float lo = scratch_[a];
    std::nth_element(scratch_.begin(), scratch_.begin() + b, scratch_.end());
    float hi = scratch_[b];
    float minSpan = f.units == Units::Celsius ? 2.f : 8.f;
    if (hi - lo < minSpan) {
        float mid = (hi + lo) / 2;
        lo = mid - minSpan / 2;
        hi = mid + minSpan / 2;
    }
    if (!gainInit_) {
        lo_ = lo;
        hi_ = hi;
        gainInit_ = true;
    } else {
        const float k = 0.15f;
        lo_ += (lo - lo_) * k;
        hi_ += (hi - hi_) * k;
    }
}

// Plateau-limited histogram equalization, the standard AGC in thermal
// cameras: spreads the colours over the temperatures that actually occur, so
// a room at 20-24 °C isn't one flat shade because a 60 °C mug is in view.
// The plateau stops big uniform areas (walls) from hogging the palette, and a
// high-pass "detail" term brings out edges and texture.
void Renderer::equalize(const ThermalFrame& f, const RenderOptions& opt) {
    constexpr int kBins = 512;
    const size_t n = f.value.size();
    const float lo = lo_, span = std::max(1e-3f, hi_ - lo_);
    std::vector<float> hist(kBins, 0.f);
    for (float v : f.value) hist[std::clamp(int((v - lo) / span * (kBins - 1)), 0, kBins - 1)] += 1.f;
    const float plateau = std::max(1.f, 4.f * n / kBins);
    float total = 0;
    for (float& h : hist) total += (h = std::min(h, plateau));
    std::vector<float> cdf(kBins);
    float acc = 0;
    for (int i = 0; i < kBins; ++i) {
        acc += hist[i];
        cdf[i] = total > 0 ? acc / total : float(i) / (kBins - 1);
    }
    // Smooth the curve over time so the colours don't pump.
    if (cdf_.size() != size_t(kBins)) cdf_ = cdf;
    else
        for (int i = 0; i < kBins; ++i) cdf_[i] += (cdf[i] - cdf_[i]) * 0.25f;

    const int w = f.width, h = f.height;
    const float detailGain = opt.detail * 8.f / span;
    unit_.resize(n);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            size_t i = size_t(y) * w + x;
            float v = f.value[i];
            float pos = std::clamp((v - lo) / span, 0.f, 1.f);
            float he = cdf_[int(pos * (kBins - 1))];
            // 3x3 local mean for the detail (high-pass) term.
            float sum = 0;
            int cnt = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    int xx = x + dx, yy = y + dy;
                    if (xx >= 0 && yy >= 0 && xx < w && yy < h) { sum += f.value[size_t(yy) * w + xx]; ++cnt; }
                }
            float detail = (v - sum / cnt) * detailGain;
            unit_[i] = std::clamp(0.85f * he + 0.15f * pos + detail, 0.f, 1.f);
        }
}

void Renderer::mapToUnit(const ThermalFrame& f, const RenderOptions& opt) {
    const size_t n = f.value.size();
    GainMode g = opt.gain;
    if (opt.fixedRange) g = GainMode::Linear;
    if (g == GainMode::Camera && f.display.size() != n) g = GainMode::Equalize;
    if (g == GainMode::Equalize && f.units != Units::Celsius && f.display.size() == n) g = GainMode::Camera;
    effGain_ = g;
    unit_.resize(n);
    switch (g) {
        case GainMode::Camera:
            for (size_t i = 0; i < n; ++i) unit_[i] = f.display[i] * (1.f / 255.f);
            break;
        case GainMode::Equalize:
            equalize(f, opt);
            break;
        default: {
            const float inv = hi_ > lo_ ? 1.f / (hi_ - lo_) : 0.f;
            for (size_t i = 0; i < n; ++i) unit_[i] = std::clamp((f.value[i] - lo_) * inv, 0.f, 1.f);
        }
    }
}

void Renderer::render(const ThermalFrame& f, const RenderOptions& opt, RgbaImage& out) {
    if (f.width <= 0 || f.height <= 0 || f.value.size() < size_t(f.width) * f.height) return;
    stats_ = computeStats(f);
    updateGain(f, opt);

    const int sc = std::max(1, opt.scale);
    const int ts = std::max(2, sc * 2 / 3);  // text scale
    const int line = std::max(1, sc / 2);
    const uint32_t white = rgba(255, 255, 255);
    const uint32_t panel = rgba(18, 18, 22);

    // Layout: the thermal image is never drawn over by text. Readouts go in
    // a strip below it and the colour scale in a column to its right.
    const int imgW = f.width * sc, imgH = f.height * sc;
    const int panelW = opt.colorBar ? textWidth("-000.0*F", ts) + 4 * ts : 0;
    const int stripH = opt.stats ? 11 * ts : 0;
    out.width = imgW + panelW;
    out.height = imgH + stripH;
    out.imageX = 0;
    out.imageY = 0;
    out.imageW = imgW;
    out.imageH = imgH;
    out.px.resize(size_t(out.width) * out.height * 4);
    if (panelW || stripH) fillRect(out, 0, 0, out.width, out.height, panel);

    const auto& lut = lutFor(opt.palette);
    mapToUnit(f, opt);

    // Bilinear interpolation of the colour position, then colour map.
    for (int oy = 0; oy < imgH; ++oy) {
        float sy = std::clamp((oy + 0.5f) / sc - 0.5f, 0.f, float(f.height - 1));
        int y0 = int(sy), y1 = std::min(y0 + 1, f.height - 1);
        float fy = sy - y0;
        const float* r0 = &unit_[size_t(y0) * f.width];
        const float* r1 = &unit_[size_t(y1) * f.width];
        uint32_t* dst = reinterpret_cast<uint32_t*>(&out.px[size_t(oy) * out.width * 4]);
        for (int ox = 0; ox < imgW; ++ox) {
            float sx = std::clamp((ox + 0.5f) / sc - 0.5f, 0.f, float(f.width - 1));
            int x0 = int(sx), x1 = std::min(x0 + 1, f.width - 1);
            float fx = sx - x0;
            float v = (r0[x0] * (1 - fx) + r0[x1] * fx) * (1 - fy) + (r1[x0] * (1 - fx) + r1[x1] * fx) * fy;
            dst[ox] = lut[std::clamp(int(v * 255.f + 0.5f), 0, 255)];  // little-endian RGBA
        }
    }

    // Thin outline markers only; no text on the image.
    if (opt.markers) {
        int r = 4 * sc, t = line + 1;
        drawBox(out, stats_.maxX * sc + sc / 2, stats_.maxY * sc + sc / 2, r, t, rgba(255, 60, 60));
        drawBox(out, stats_.minX * sc + sc / 2, stats_.minY * sc + sc / 2, r, t, rgba(80, 160, 255));
    }
    if (opt.crosshair) {
        int cx = imgW / 2, cy = imgH / 2;
        drawCross(out, cx, cy, 4 * sc, line + 2, rgba(0, 0, 0, 140));
        drawCross(out, cx, cy, 4 * sc - 1, line, white);
    }
    if (opt.stats) {
        const int y = imgH + 2 * ts;
        int x = 2 * ts;
        auto field = [&](const char* name, float v, uint32_t c) {
            drawText(out, x, y, name, ts, c);
            x += textWidth(name, ts) + 5 * ts;
            std::string val = fmtValue(v, f.units, opt.fahrenheit);
            drawText(out, x, y, val, ts, white);
            x += textWidth(val, ts) + 5 * ts;
        };
        field("MAX", stats_.max, rgba(255, 110, 110));
        field("MIN", stats_.min, rgba(120, 180, 255));
        if (opt.crosshair) field("CTR", stats_.center, white);
    }
    if (opt.colorBar) {
        // Non-linear modes don't map colour to a fixed temperature scale, so
        // label the ends with the scene's actual extremes instead.
        bool lin = effGain_ == GainMode::Linear;
        std::string hi = fmtValue(lin ? hi_ : stats_.max, f.units, opt.fahrenheit);
        std::string lo = fmtValue(lin ? lo_ : stats_.min, f.units, opt.fahrenheit);
        int bw = 3 * sc;
        int bx = imgW + (panelW - bw) / 2;
        int by = 11 * ts, bh = std::max(1, imgH - 22 * ts);
        drawText(out, imgW + (panelW - textWidth(hi, ts)) / 2, 2 * ts, hi, ts, white);
        drawText(out, imgW + (panelW - textWidth(lo, ts)) / 2, by + bh + 2 * ts, lo, ts, white);
        for (int y = 0; y < bh; ++y) fillRect(out, bx, by + y, bw, 1, lut[255 - y * 255 / std::max(1, bh - 1)]);
    }
}

}  // namespace thermal
