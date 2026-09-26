// Decoding raw camera frames into per-pixel thermal values.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../uvc/uvc_descriptors.h"

namespace thermal {

enum class Units { Celsius, Raw };

struct ThermalFrame {
    int width = 0, height = 0;
    Units units = Units::Raw;
    std::vector<float> value;  // row-major, width*height
    // The camera's own processed 8-bit picture (0..255), when it sends one.
    // Cheap thermal cameras run detail enhancement on this image, so it shows
    // far more texture than a plain mapping of the temperatures.
    std::vector<float> display;
    uint64_t seq = 0;
};

enum class DecodeMode {
    Auto,
    Grey,          // YUYV luma or Y8: brightness only
    Y16Raw,        // 16-bit counts without calibration
    Y16CentiKelvin,// 16-bit, value/100 = Kelvin (FLIR Lepton radiometric / PureThermal)
    SplitInfiray,  // YUYV WxH where top half is image and bottom half is 1/64 K temperatures
};

const char* decodeModeName(DecodeMode m);
bool parseDecodeMode(const std::string& s, DecodeMode& out);

// Resolves Auto to a concrete mode from the stream's format and size.
DecodeMode resolveMode(DecodeMode requested, uvc::PixelFormat fmt, int width, int height);

// Decodes one raw frame. Returns false if the buffer is too small or the mode
// doesn't apply to the pixel format.
bool decode(const uint8_t* data, size_t len, uvc::PixelFormat fmt, int width, int height, DecodeMode mode,
            ThermalFrame& out);

// Orientation fixes for how the camera is mounted.
void transform(ThermalFrame& f, bool flipH, bool flipV, int rotateDeg);

}  // namespace thermal
