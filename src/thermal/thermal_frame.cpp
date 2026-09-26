#include "thermal_frame.h"

#include <algorithm>

namespace thermal {

constexpr float kKelvinOffset = 273.15f;

const char* decodeModeName(DecodeMode m) {
    switch (m) {
        case DecodeMode::Auto: return "auto";
        case DecodeMode::Grey: return "grey";
        case DecodeMode::Y16Raw: return "y16";
        case DecodeMode::Y16CentiKelvin: return "y16-ck";
        case DecodeMode::SplitInfiray: return "split";
    }
    return "?";
}

bool parseDecodeMode(const std::string& s, DecodeMode& out) {
    for (DecodeMode m : {DecodeMode::Auto, DecodeMode::Grey, DecodeMode::Y16Raw, DecodeMode::Y16CentiKelvin,
                         DecodeMode::SplitInfiray}) {
        if (s == decodeModeName(m)) {
            out = m;
            return true;
        }
    }
    return false;
}

DecodeMode resolveMode(DecodeMode requested, uvc::PixelFormat fmt, int width, int height) {
    if (requested != DecodeMode::Auto) return requested;
    using uvc::PixelFormat;
    if (fmt == PixelFormat::YUYV && height * 2 == width * 3) return DecodeMode::SplitInfiray;
    if (fmt == PixelFormat::Y16) {
        // PureThermal / Lepton 3.x radiometric streams are 160x120 in centikelvin.
        if ((width == 160 && height == 120) || (width == 80 && height == 60)) return DecodeMode::Y16CentiKelvin;
        return DecodeMode::Y16Raw;
    }
    return DecodeMode::Grey;
}

static inline uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

bool decode(const uint8_t* data, size_t len, uvc::PixelFormat fmt, int width, int height, DecodeMode mode,
            ThermalFrame& out) {
    using uvc::PixelFormat;
    mode = resolveMode(mode, fmt, width, height);
    const size_t n = size_t(width) * height;

    switch (mode) {
        case DecodeMode::SplitInfiray: {
            if (fmt != PixelFormat::YUYV && fmt != PixelFormat::Y16) return false;
            if (len < n * 2 || height % 2) return false;
            out.width = width;
            out.height = height / 2;
            out.units = Units::Celsius;
            out.value.resize(size_t(out.width) * out.height);
            const uint8_t* t = data + size_t(width) * out.height * 2;  // bottom half
            for (size_t i = 0; i < out.value.size(); ++i) out.value[i] = le16(t + 2 * i) / 64.0f - kKelvinOffset;
            out.display.resize(out.value.size());
            if (fmt == PixelFormat::YUYV)
                for (size_t i = 0; i < out.display.size(); ++i) out.display[i] = data[2 * i];  // luma of top half
            else
                out.display.clear();
            return true;
        }
        case DecodeMode::Y16Raw:
        case DecodeMode::Y16CentiKelvin: {
            if (bytesPerPixel(fmt) != 2 || len < n * 2) return false;
            out.width = width;
            out.height = height;
            out.display.clear();
            out.value.resize(n);
            if (mode == DecodeMode::Y16CentiKelvin) {
                out.units = Units::Celsius;
                for (size_t i = 0; i < n; ++i) out.value[i] = le16(data + 2 * i) / 100.0f - kKelvinOffset;
            } else {
                out.units = Units::Raw;
                for (size_t i = 0; i < n; ++i) out.value[i] = le16(data + 2 * i);
            }
            return true;
        }
        case DecodeMode::Grey:
        default: {
            out.width = width;
            out.height = height;
            out.units = Units::Raw;
            out.display.clear();
            out.value.resize(n);
            if (fmt == PixelFormat::YUYV) {
                if (len < n * 2) return false;
                for (size_t i = 0; i < n; ++i) out.value[i] = data[2 * i];
            } else if (fmt == PixelFormat::UYVY) {
                if (len < n * 2) return false;
                for (size_t i = 0; i < n; ++i) out.value[i] = data[2 * i + 1];
            } else if (fmt == PixelFormat::Y8) {
                if (len < n) return false;
                for (size_t i = 0; i < n; ++i) out.value[i] = data[i];
            } else if (fmt == PixelFormat::Y16) {
                if (len < n * 2) return false;
                for (size_t i = 0; i < n; ++i) out.value[i] = le16(data + 2 * i);
            } else {
                return false;
            }
            return true;
        }
    }
}

static void transformPlane(std::vector<float>& v, int w, int h, bool flipH, bool flipV, int rotateDeg) {
    if (v.size() != size_t(w) * h) return;
    if (flipH)
        for (int y = 0; y < h; ++y) std::reverse(v.begin() + y * w, v.begin() + (y + 1) * w);
    if (flipV)
        for (int y = 0; y < h / 2; ++y)
            std::swap_ranges(v.begin() + y * w, v.begin() + (y + 1) * w, v.begin() + (h - 1 - y) * w);
    if (rotateDeg == 90 || rotateDeg == 270) {
        std::vector<float> r(v.size());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                // 90 = clockwise
                int nx = rotateDeg == 90 ? h - 1 - y : y;
                int ny = rotateDeg == 90 ? x : w - 1 - x;
                r[size_t(ny) * h + nx] = v[size_t(y) * w + x];
            }
        v.swap(r);
    }
}

void transform(ThermalFrame& f, bool flipH, bool flipV, int rotateDeg) {
    rotateDeg = ((rotateDeg % 360) + 360) % 360;
    if (rotateDeg == 180) {
        flipH = !flipH;
        flipV = !flipV;
        rotateDeg = 0;
    }
    transformPlane(f.value, f.width, f.height, flipH, flipV, rotateDeg);
    transformPlane(f.display, f.width, f.height, flipH, flipV, rotateDeg);
    if (rotateDeg == 90 || rotateDeg == 270) std::swap(f.width, f.height);
}

}  // namespace thermal
