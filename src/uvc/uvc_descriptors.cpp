#include "uvc_descriptors.h"

#include <cstdio>
#include <cstring>

namespace uvc {
namespace {

constexpr uint8_t CS_INTERFACE = 0x24;

// VideoControl subtypes
constexpr uint8_t VC_HEADER = 0x01;

// VideoStreaming subtypes
constexpr uint8_t VS_INPUT_HEADER = 0x01;
constexpr uint8_t VS_FORMAT_UNCOMPRESSED = 0x04;
constexpr uint8_t VS_FRAME_UNCOMPRESSED = 0x05;
constexpr uint8_t VS_FORMAT_MJPEG = 0x06;
constexpr uint8_t VS_FRAME_MJPEG = 0x07;
constexpr uint8_t VS_FORMAT_FRAME_BASED = 0x10;
constexpr uint8_t VS_FRAME_FRAME_BASED = 0x11;

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Walks a buffer of concatenated descriptors, calling fn for each
// well-formed class-specific interface descriptor.
template <typename Fn>
void forEachCsInterface(const uint8_t* data, int len, Fn fn) {
    int off = 0;
    while (off + 2 <= len) {
        int dlen = data[off];
        if (dlen < 2 || off + dlen > len) break;  // malformed: stop
        if (data[off + 1] == CS_INTERFACE && dlen >= 3) fn(data + off, dlen);
        off += dlen;
    }
}

}  // namespace

const char* pixelFormatName(PixelFormat f) {
    switch (f) {
        case PixelFormat::YUYV: return "YUYV";
        case PixelFormat::UYVY: return "UYVY";
        case PixelFormat::Y8: return "Y8";
        case PixelFormat::Y16: return "Y16";
        case PixelFormat::NV12: return "NV12";
        case PixelFormat::MJPEG: return "MJPEG";
        default: return "unknown";
    }
}

PixelFormat pixelFormatFromGuid(const uint8_t guid[16]) {
    // UVC GUIDs are FOURCC + 0000-0010-8000-00AA00389B71
    char cc[5] = {char(guid[0]), char(guid[1]), char(guid[2]), char(guid[3]), 0};
    if (!strcmp(cc, "YUY2") || !strcmp(cc, "YUYV")) return PixelFormat::YUYV;
    if (!strcmp(cc, "UYVY")) return PixelFormat::UYVY;
    if (!strcmp(cc, "Y800") || !strcmp(cc, "Y8  ") || !strcmp(cc, "GREY")) return PixelFormat::Y8;
    if (!strcmp(cc, "Y16 ") || !strcmp(cc, "Y12 ")) return PixelFormat::Y16;
    if (!strcmp(cc, "NV12")) return PixelFormat::NV12;
    if (!strcmp(cc, "MJPG")) return PixelFormat::MJPEG;
    return PixelFormat::Unknown;
}

int bytesPerPixel(PixelFormat f) {
    switch (f) {
        case PixelFormat::YUYV:
        case PixelFormat::UYVY:
        case PixelFormat::Y16: return 2;
        case PixelFormat::Y8: return 1;
        default: return 0;
    }
}

uint16_t parseVideoControl(const uint8_t* data, int len) {
    uint16_t bcd = 0;
    forEachCsInterface(data, len, [&](const uint8_t* d, int dlen) {
        if (d[2] == VC_HEADER && dlen >= 5 && bcd == 0) bcd = rd16(d + 3);
    });
    return bcd;
}

StreamingDesc parseVideoStreaming(const uint8_t* data, int len) {
    StreamingDesc out;
    forEachCsInterface(data, len, [&](const uint8_t* d, int dlen) {
        switch (d[2]) {
            case VS_INPUT_HEADER:
                if (dlen >= 7) out.endpointAddress = d[6];
                break;
            case VS_FORMAT_UNCOMPRESSED:
            case VS_FORMAT_FRAME_BASED: {
                if (dlen < 23) break;
                FormatDesc f;
                f.index = d[3];
                memcpy(f.guid, d + 5, 16);
                f.format = pixelFormatFromGuid(f.guid);
                f.bitsPerPixel = d[21];
                f.defaultFrameIndex = d[22];
                out.formats.push_back(f);
                break;
            }
            case VS_FORMAT_MJPEG: {
                if (dlen < 6) break;
                FormatDesc f;
                f.index = d[3];
                f.format = PixelFormat::MJPEG;
                f.defaultFrameIndex = d[5];
                out.formats.push_back(f);
                break;
            }
            case VS_FRAME_UNCOMPRESSED:
            case VS_FRAME_MJPEG:
            case VS_FRAME_FRAME_BASED: {
                if (dlen < 26 || out.formats.empty()) break;
                FrameDesc fr;
                fr.index = d[3];
                fr.width = rd16(d + 5);
                fr.height = rd16(d + 7);
                // Frame-based descriptors have no dwMaxVideoFrameBufferSize, so
                // the interval fields sit 4 bytes earlier, then dwBytesPerLine
                // brings the interval table back to offset 26 in both layouts.
                int o = (d[2] == VS_FRAME_FRAME_BASED) ? 17 : 21;
                if (d[2] != VS_FRAME_FRAME_BASED) fr.maxFrameBufferSize = rd32(d + 17);
                fr.defaultInterval = rd32(d + o);
                uint8_t type = d[o + 4];
                int p = 26;
                if (type == 0) {
                    if (p + 12 <= dlen) {
                        fr.minInterval = rd32(d + p);
                        fr.maxInterval = rd32(d + p + 4);
                        fr.stepInterval = rd32(d + p + 8);
                    }
                } else {
                    for (int i = 0; i < type && p + 4 <= dlen; ++i, p += 4) fr.intervals.push_back(rd32(d + p));
                }
                out.formats.back().frames.push_back(fr);
                break;
            }
            default: break;
        }
    });
    return out;
}

std::string describe(const StreamingDesc& s) {
    std::string r;
    char line[256];
    for (const auto& f : s.formats) {
        snprintf(line, sizeof line, "  format %u: %s (%c%c%c%c) %u bpp\n", f.index, pixelFormatName(f.format),
                 f.guid[0] ? f.guid[0] : '?', f.guid[1] ? f.guid[1] : '?', f.guid[2] ? f.guid[2] : '?',
                 f.guid[3] ? f.guid[3] : '?', f.bitsPerPixel);
        r += line;
        for (const auto& fr : f.frames) {
            double fps = fr.defaultInterval ? 1e7 / fr.defaultInterval : 0;
            snprintf(line, sizeof line, "    frame %u: %ux%u  default %.1f fps", fr.index, fr.width, fr.height, fps);
            r += line;
            if (!fr.intervals.empty()) {
                r += "  [";
                for (size_t i = 0; i < fr.intervals.size(); ++i) {
                    snprintf(line, sizeof line, "%s%.1f", i ? " " : "", fr.intervals[i] ? 1e7 / fr.intervals[i] : 0);
                    r += line;
                }
                r += " fps]";
            }
            r += "\n";
        }
    }
    return r;
}

int probeControlSize(uint16_t bcdUVC) {
    if (bcdUVC >= 0x0150) return 48;
    if (bcdUVC >= 0x0110) return 34;
    return 26;
}

}  // namespace uvc
