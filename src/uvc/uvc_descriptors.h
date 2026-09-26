// Parsing of USB Video Class (UVC) class-specific descriptors.
//
// These are pure functions over raw descriptor bytes so they can be unit
// tested without hardware. libusb hands us the class-specific descriptors in
// the `extra` field of the interface / endpoint descriptors.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace uvc {

enum class PixelFormat {
    Unknown,
    YUYV,   // YUY2, 16 bpp packed 4:2:2
    UYVY,
    Y8,     // 8-bit greyscale
    Y16,    // 16-bit little-endian greyscale / raw sensor counts
    NV12,
    MJPEG,
};

const char* pixelFormatName(PixelFormat f);
PixelFormat pixelFormatFromGuid(const uint8_t guid[16]);
int bytesPerPixel(PixelFormat f);  // 0 for compressed / planar formats

struct FrameDesc {
    uint8_t index = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t maxFrameBufferSize = 0;
    uint32_t defaultInterval = 0;          // 100 ns units
    std::vector<uint32_t> intervals;       // discrete intervals, 100 ns units
    uint32_t minInterval = 0, maxInterval = 0, stepInterval = 0;  // continuous
};

struct FormatDesc {
    uint8_t index = 0;
    PixelFormat format = PixelFormat::Unknown;
    uint8_t guid[16] = {};
    uint8_t bitsPerPixel = 0;
    uint8_t defaultFrameIndex = 0;
    std::vector<FrameDesc> frames;
};

// Parses the VideoControl interface's class-specific descriptors.
// Returns the bcdUVC version (e.g. 0x0100, 0x0110, 0x0150) or 0 if not found.
uint16_t parseVideoControl(const uint8_t* data, int len);

struct StreamingDesc {
    uint8_t endpointAddress = 0;  // from VS_INPUT_HEADER
    std::vector<FormatDesc> formats;
};

// Parses the VideoStreaming interface's class-specific descriptors
// (VS_INPUT_HEADER, VS_FORMAT_*, VS_FRAME_*).
StreamingDesc parseVideoStreaming(const uint8_t* data, int len);

std::string describe(const StreamingDesc& s);

// Size in bytes of the VS probe/commit control block for a UVC version.
int probeControlSize(uint16_t bcdUVC);

}  // namespace uvc
