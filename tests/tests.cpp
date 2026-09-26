// Hardware-free tests for the UVC parsing, frame assembly, decoding and rendering.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/app/sources.h"
#include "../src/thermal/renderer.h"
#include "../src/thermal/thermal_frame.h"
#include "../src/uvc/frame_assembler.h"
#include "../src/uvc/uvc_camera.h"
#include "../src/uvc/uvc_descriptors.h"
#include "../src/util/png.h"

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

static void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xff); v.push_back(x >> 8); }
static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xff); }

// VideoStreaming descriptors shaped like an InfiRay P2 Pro: one YUYV format
// with 256x384 (image + temperature) and 256x192 frames.
static std::vector<uint8_t> infirayVsDescriptors() {
    std::vector<uint8_t> d;
    // VS_INPUT_HEADER (13 bytes, 1 format)
    d.insert(d.end(), {13, 0x24, 0x01, 1});
    put16(d, 0);          // wTotalLength (unused by parser)
    d.push_back(0x81);    // bEndpointAddress
    d.insert(d.end(), {0, 0, 0, 0, 1, 0});
    // VS_FORMAT_UNCOMPRESSED
    d.insert(d.end(), {27, 0x24, 0x04, 1, 2});
    const uint8_t guid[16] = {'Y', 'U', 'Y', '2', 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
    d.insert(d.end(), guid, guid + 16);
    d.insert(d.end(), {16, 1, 0, 0, 0, 0});
    auto frame = [&](uint8_t idx, uint16_t w, uint16_t h, std::vector<uint32_t> ivs) {
        d.push_back(uint8_t(26 + 4 * ivs.size()));
        d.insert(d.end(), {0x24, 0x05, idx, 0});
        put16(d, w);
        put16(d, h);
        put32(d, w * h * 16 * 25);
        put32(d, w * h * 16 * 25);
        put32(d, w * h * 2);
        put32(d, ivs[0]);
        d.push_back(uint8_t(ivs.size()));
        for (uint32_t iv : ivs) put32(d, iv);
    };
    frame(1, 256, 384, {400000});
    frame(2, 256, 192, {400000, 1000000});
    return d;
}

static void testDescriptors() {
    auto d = infirayVsDescriptors();
    uvc::StreamingDesc s = uvc::parseVideoStreaming(d.data(), int(d.size()));
    CHECK(s.endpointAddress == 0x81);
    CHECK(s.formats.size() == 1);
    if (s.formats.size() != 1) return;
    const auto& f = s.formats[0];
    CHECK(f.format == uvc::PixelFormat::YUYV);
    CHECK(f.bitsPerPixel == 16);
    CHECK(f.frames.size() == 2);
    if (f.frames.size() != 2) return;
    CHECK(f.frames[0].width == 256 && f.frames[0].height == 384);
    CHECK(f.frames[0].defaultInterval == 400000);
    CHECK(f.frames[1].intervals.size() == 2 && f.frames[1].intervals[1] == 1000000);

    // Truncated / garbage input must not crash or read out of bounds.
    for (size_t n = 0; n < d.size(); ++n) uvc::parseVideoStreaming(d.data(), int(n));
    uint8_t junk[5] = {0, 0x24, 1, 2, 3};
    CHECK(uvc::parseVideoStreaming(junk, 5).formats.empty());

    uint8_t vc[] = {13, 0x24, 0x01, 0x10, 0x01, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(uvc::parseVideoControl(vc, sizeof vc) == 0x0110);
    CHECK(uvc::probeControlSize(0x0100) == 26 && uvc::probeControlSize(0x0110) == 34 &&
          uvc::probeControlSize(0x0150) == 48);
}


// Byte-for-byte the VideoStreaming descriptors from Conor's Topdon TC001
// (0bda:5830, `lsusb -v`), including the still-image and colour-format
// descriptors the parser must skip.
static std::vector<uint8_t> tc001VsDescriptors() {
    std::vector<uint8_t> d = {14, 0x24, 0x01, 1, 0x79, 0x00, 0x81, 0, 3, 2, 1, 0, 1, 0};
    d.insert(d.end(), {27, 0x24, 0x04, 1, 2});
    const uint8_t guid[16] = {0x59, 0x55, 0x59, 0x32, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
    d.insert(d.end(), guid, guid + 16);
    d.insert(d.end(), {16, 1, 0, 0, 0, 0});
    auto frame = [&](uint8_t idx, uint16_t w, uint16_t h, uint32_t rate, uint32_t buf) {
        d.insert(d.end(), {30, 0x24, 0x05, idx, 0});
        put16(d, w);
        put16(d, h);
        put32(d, rate);
        put32(d, rate);
        put32(d, buf);
        put32(d, 400000);
        d.push_back(1);
        put32(d, 400000);
    };
    frame(1, 256, 192, 19660800, 98304);
    frame(2, 256, 384, 39321600, 196608);
    d.insert(d.end(), {14, 0x24, 0x03, 0x00, 2});
    put16(d, 256); put16(d, 192); put16(d, 256); put16(d, 384);
    d.push_back(0);
    d.insert(d.end(), {6, 0x24, 0x0D, 1, 1, 4});
    return d;
}

static void testTc001() {
    auto d = tc001VsDescriptors();
    uvc::StreamingDesc s = uvc::parseVideoStreaming(d.data(), int(d.size()));
    CHECK(s.endpointAddress == 0x81);
    CHECK(s.formats.size() == 1);
    if (s.formats.size() != 1) return;
    CHECK(s.formats[0].format == uvc::PixelFormat::YUYV);
    CHECK(s.formats[0].frames.size() == 2);

    // Default choice must be the 256x384 image+temperature frame at 25 fps.
    uvc::FormatDesc f;
    uvc::FrameDesc fr;
    CHECK(uvc::chooseFormat(s, uvc::StreamRequest{}, f, fr));
    CHECK(f.index == 1 && fr.index == 2 && fr.width == 256 && fr.height == 384);
    CHECK(fr.defaultInterval == 400000);
    CHECK(thermal::resolveMode(thermal::DecodeMode::Auto, f.format, fr.width, fr.height) ==
          thermal::DecodeMode::SplitInfiray);

    // TC001 alternate settings (bytes per microframe incl. high-bandwidth mult).
    std::vector<std::pair<int, int>> alts = {{1, 128}, {2, 512}, {3, 1024}, {4, 1536}, {5, 2048}, {6, 2688}, {7, 3072}};
    CHECK(uvc::chooseIsoAlt(alts, 3072) == 7);
    CHECK(uvc::chooseIsoAlt(alts, 1000) == 3);
    CHECK(uvc::chooseIsoAlt(alts, 0) == 7);
    CHECK(uvc::chooseIsoAlt(alts, 9000) == 7);
    CHECK(uvc::chooseIsoAlt({}, 1000) == -1);
    CHECK(uvc::probeControlSize(0x0100) == 26);  // TC001 reports bcdUVC 1.00
}

static std::vector<uint8_t> payload(uint8_t info, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> p = {2, info};
    p.insert(p.end(), data.begin(), data.end());
    return p;
}

static void testAssembler() {
    std::vector<std::vector<uint8_t>> got;
    uvc::FrameAssembler a(6);
    a.setCallback([&](const uint8_t* d, size_t n) { got.emplace_back(d, d + n); });

    // EOF-terminated frame.
    auto p1 = payload(0x00, {1, 2, 3});
    auto p2 = payload(0x02, {4, 5, 6});
    a.push(p1.data(), p1.size());
    a.push(p2.data(), p2.size());
    CHECK(got.size() == 1 && got[0] == std::vector<uint8_t>({1, 2, 3, 4, 5, 6}));

    // Frame ended only by FID toggle (no EOF bit).
    auto p3 = payload(0x01, {7, 8, 9});
    auto p4 = payload(0x01, {10, 11, 12});
    auto p5 = payload(0x00, {1});
    a.push(p3.data(), p3.size());
    a.push(p4.data(), p4.size());
    a.push(p5.data(), p5.size());
    CHECK(got.size() == 2 && got[1][0] == 7 && got[1][5] == 12);

    // Short frame (lost packets) is dropped, not delivered.
    auto p6 = payload(0x02, {2});
    a.push(p6.data(), p6.size());
    CHECK(got.size() == 2 && a.framesDropped() == 1);

    // Error bit poisons the frame.
    auto p7 = payload(0x41, {1, 2, 3});
    auto p8 = payload(0x03, {4, 5, 6});
    a.push(p7.data(), p7.size());
    a.push(p8.data(), p8.size());
    CHECK(got.size() == 2);

    // Bad headers are counted and ignored.
    uint8_t bad[] = {9, 0};
    a.push(bad, 2);
    CHECK(a.badHeaders() == 1);
}

static void testSplitDecode() {
    const int W = 4, H = 6;  // 4x3 image + 4x3 temperatures
    std::vector<uint8_t> raw(W * H * 2, 0);
    for (int i = 0; i < W * 3; ++i) {
        uint16_t v = uint16_t(std::lround((20.0 + i + 273.15) * 64));
        raw[W * 3 * 2 + 2 * i] = v & 0xff;
        raw[W * 3 * 2 + 2 * i + 1] = v >> 8;
    }
    thermal::ThermalFrame f;
    CHECK(thermal::resolveMode(thermal::DecodeMode::Auto, uvc::PixelFormat::YUYV, 256, 384) ==
          thermal::DecodeMode::SplitInfiray);
    CHECK(thermal::decode(raw.data(), raw.size(), uvc::PixelFormat::YUYV, W, H, thermal::DecodeMode::SplitInfiray, f));
    CHECK(f.width == 4 && f.height == 3 && f.units == thermal::Units::Celsius);
    CHECK(std::fabs(f.value[0] - 20.0f) < 0.02f);
    CHECK(std::fabs(f.value[11] - 31.0f) < 0.02f);
    CHECK(!thermal::decode(raw.data(), raw.size() - 1, uvc::PixelFormat::YUYV, W, H, thermal::DecodeMode::SplitInfiray, f));

    // Rotation / flips.
    thermal::ThermalFrame t;
    t.width = 3;
    t.height = 2;
    t.value = {1, 2, 3, 4, 5, 6};
    thermal::transform(t, false, false, 90);
    CHECK(t.width == 2 && t.height == 3);
    CHECK((t.value == std::vector<float>{4, 1, 5, 2, 6, 3}));
    thermal::transform(t, false, false, 270);
    CHECK((t.value == std::vector<float>{1, 2, 3, 4, 5, 6}));
    thermal::transform(t, false, false, 180);
    CHECK((t.value == std::vector<float>{6, 5, 4, 3, 2, 1}));
}

// Full pipeline: simulated camera -> UVC payloads -> assembler -> decoder -> renderer.
static void testSimPipeline() {
    FrameMailbox box;
    SimSource sim(60);
    CHECK(sim.start(box));
    RawFrame raw;
    int got = 0;
    thermal::ThermalFrame tf;
    thermal::Renderer r;
    thermal::RgbaImage img;
    thermal::RenderOptions opt;
    for (int i = 0; i < 100 && got < 5; ++i) {
        if (!box.take(raw, 100)) continue;
        ++got;
        CHECK(raw.width == 256 && raw.height == 384 && raw.format == uvc::PixelFormat::YUYV);
        CHECK(thermal::decode(raw.data.data(), raw.data.size(), raw.format, raw.width, raw.height,
                              thermal::DecodeMode::Auto, tf));
        opt.gain = thermal::GainMode(got % int(thermal::GainMode::Count));
        r.render(tf, opt, img);
        CHECK(r.effectiveGain() == opt.gain);
    }
    sim.stop();
    CHECK(got == 5);
    CHECK(tf.width == 256 && tf.height == 192);
    const auto& s = r.lastStats();
    CHECK(s.max > 55 && s.max < 65);  // the mug
    CHECK(s.min > 12 && s.min < 16);  // the window
    // Image is 3x the sensor; readouts sit in a border outside it.
    CHECK(img.imageW == 768 && img.imageH == 576);
    CHECK(img.width > img.imageW && img.height > img.imageH);
    CHECK(writePng("/tmp/thermal-test.png", img.px.data(), img.width, img.height));
    FILE* f = fopen("/tmp/thermal-test.png", "rb");
    uint8_t sig[8] = {};
    if (f) {
        CHECK(fread(sig, 1, 8, f) == 8);
        fclose(f);
    }
    CHECK(sig[0] == 0x89 && sig[1] == 'P');
}

int main() {
    testDescriptors();
    testTc001();
    testAssembler();
    testSplitDecode();
    testSimPipeline();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
