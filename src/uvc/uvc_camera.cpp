#include "uvc_camera.h"

#include <libusb.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace uvc {
namespace {

constexpr uint8_t CC_VIDEO = 0x0e;
constexpr uint8_t SC_VIDEOCONTROL = 0x01;
constexpr uint8_t SC_VIDEOSTREAMING = 0x02;

constexpr uint8_t SET_CUR = 0x01;
constexpr uint8_t GET_CUR = 0x81;
constexpr uint8_t VS_PROBE_CONTROL = 0x01;
constexpr uint8_t VS_COMMIT_CONTROL = 0x02;

constexpr int kCtrlTimeoutMs = 1000;
constexpr int kIsoPacketsPerTransfer = 32;
constexpr int kIsoTransfers = 10;
constexpr int kBulkTransfers = 5;

void wr16(uint8_t* p, uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
void wr32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (v >> (8 * i)) & 0xff; }
uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

struct Interfaces {
    int vc = -1, vs = -1;
    uint16_t bcd = 0;
    StreamingDesc streaming;
};

// Finds the first VideoControl/VideoStreaming pair in a configuration and
// parses their class-specific descriptors.
Interfaces findInterfaces(const libusb_config_descriptor* cfg) {
    Interfaces r;
    for (int i = 0; i < cfg->bNumInterfaces; ++i) {
        const libusb_interface& itf = cfg->interface[i];
        if (itf.num_altsetting < 1) continue;
        const libusb_interface_descriptor& alt0 = itf.altsetting[0];
        if (alt0.bInterfaceClass != CC_VIDEO) continue;
        if (alt0.bInterfaceSubClass == SC_VIDEOCONTROL && r.vc < 0) {
            r.vc = alt0.bInterfaceNumber;
            r.bcd = parseVideoControl(alt0.extra, alt0.extra_length);
        } else if (alt0.bInterfaceSubClass == SC_VIDEOSTREAMING && r.vs < 0) {
            // Class-specific VS descriptors normally live in the interface's
            // extra bytes, but some devices attach them to the endpoint.
            std::vector<uint8_t> extra(alt0.extra, alt0.extra + alt0.extra_length);
            for (int e = 0; e < alt0.bNumEndpoints; ++e)
                extra.insert(extra.end(), alt0.endpoint[e].extra, alt0.endpoint[e].extra + alt0.endpoint[e].extra_length);
            StreamingDesc s = parseVideoStreaming(extra.data(), int(extra.size()));
            if (s.formats.empty()) continue;
            r.vs = alt0.bInterfaceNumber;
            r.streaming = std::move(s);
        }
    }
    return r;
}

int isoPacketSize(const libusb_endpoint_descriptor& ep) {
    int base = ep.wMaxPacketSize & 0x7ff;
    int mult = ((ep.wMaxPacketSize >> 11) & 3) + 1;
    return base * mult;
}

uint32_t chooseInterval(const FrameDesc& fr, double fps) {
    if (fps <= 0) return fr.defaultInterval;
    uint32_t want = uint32_t(1e7 / fps);
    if (!fr.intervals.empty()) {
        uint32_t best = fr.intervals[0];
        for (uint32_t iv : fr.intervals)
            if (std::labs(long(iv) - long(want)) < std::labs(long(best) - long(want))) best = iv;
        return best;
    }
    if (fr.minInterval && want < fr.minInterval) return fr.minInterval;
    if (fr.maxInterval && want > fr.maxInterval) return fr.maxInterval;
    return want;
}

}  // namespace

// Picks the (format, frame) pair to stream. Thermal cameras are recognised by
// their shapes: the InfiRay/Topdon family exposes a YUYV frame that is 1.5x as
// tall as it is wide (image on top, temperature data below).
bool chooseFormat(const StreamingDesc& s, const StreamRequest& req, FormatDesc& fOut, FrameDesc& frOut) {
    long bestScore = -1;
    for (const auto& f : s.formats) {
        if (req.formatIndex && f.index != req.formatIndex) continue;
        if (bytesPerPixel(f.format) == 0) continue;  // no decoder for compressed formats
        for (const auto& fr : f.frames) {
            if (req.frameIndex && fr.index != req.frameIndex) continue;
            long score = long(fr.width) * fr.height / 1000;
            if (req.width && req.height) {
                if (fr.width != req.width || fr.height != req.height) continue;
                score += 100000;
            }
            if (f.format == PixelFormat::YUYV && fr.height * 2 == fr.width * 3) score += 10000;
            if (f.format == PixelFormat::Y16) score += 5000;
            if (score > bestScore) {
                bestScore = score;
                fOut = f;
                frOut = fr;
            }
        }
    }
    return bestScore >= 0;
}

int chooseIsoAlt(const std::vector<std::pair<int, int>>& alts, uint32_t maxPayload) {
    int best = -1, bestSize = 0, largest = -1, largestSize = 0;
    for (const auto& a : alts) {
        if (a.second > largestSize) { largestSize = a.second; largest = a.first; }
        if (maxPayload && a.second >= int(maxPayload) && (best < 0 || a.second < bestSize)) {
            bestSize = a.second;
            best = a.first;
        }
    }
    // Device reported 0 or an unsatisfiable payload size: use the widest pipe.
    return best >= 0 ? best : largest;
}

UvcCamera::UvcCamera() {
    if (libusb_init(&ctx_) != 0) ctx_ = nullptr;
}

UvcCamera::~UvcCamera() {
    close();
    if (ctx_) libusb_exit(ctx_);
}

bool UvcCamera::fail(const std::string& msg) {
    err_ = msg;
    return false;
}

std::vector<DeviceSummary> UvcCamera::enumerate() {
    std::vector<DeviceSummary> out;
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) return out;
    libusb_device** list = nullptr;
    ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n; ++i) {
        libusb_device* dev = list[i];
        libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(dev, &dd) != 0) continue;
        DeviceSummary s;
        s.vid = dd.idVendor;
        s.pid = dd.idProduct;
        s.bus = libusb_get_bus_number(dev);
        s.address = libusb_get_device_address(dev);
        libusb_config_descriptor* cfg = nullptr;
        if (libusb_get_active_config_descriptor(dev, &cfg) == 0 ||
            libusb_get_config_descriptor(dev, 0, &cfg) == 0) {
            Interfaces itf = findInterfaces(cfg);
            s.isUvc = itf.vc >= 0 && itf.vs >= 0;
            s.bcdUVC = itf.bcd;
            s.streaming = itf.streaming;
            libusb_free_config_descriptor(cfg);
        }
        libusb_device_handle* h = nullptr;
        int rc = libusb_open(dev, &h);
        if (rc == 0) {
            unsigned char buf[256];
            if (dd.iProduct && libusb_get_string_descriptor_ascii(h, dd.iProduct, buf, sizeof buf) > 0)
                s.product = reinterpret_cast<char*>(buf);
            libusb_close(h);
        } else {
            s.openError = libusb_error_name(rc);
        }
        out.push_back(std::move(s));
    }
    if (list) libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return out;
}

bool UvcCamera::open(uint16_t vid, uint16_t pid) {
    close();
    if (!ctx_) return fail("libusb_init failed");
    libusb_device** list = nullptr;
    ssize_t n = libusb_get_device_list(ctx_, &list);
    libusb_device* chosen = nullptr;
    Interfaces itf;
    for (ssize_t i = 0; i < n && !chosen; ++i) {
        libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
        if (vid && dd.idVendor != vid) continue;
        if (pid && dd.idProduct != pid) continue;
        libusb_config_descriptor* cfg = nullptr;
        if (libusb_get_active_config_descriptor(list[i], &cfg) != 0) continue;
        Interfaces cand = findInterfaces(cfg);
        libusb_free_config_descriptor(cfg);
        if (cand.vc < 0 || cand.vs < 0) continue;
        chosen = list[i];
        itf = cand;
    }
    if (!chosen) {
        if (list) libusb_free_device_list(list, 1);
        char msg[128];
        snprintf(msg, sizeof msg, "no UVC device found%s%04x:%04x", vid ? " matching " : "", vid, pid);
        return fail(vid ? msg : "no UVC device found");
    }
    int rc = libusb_open(chosen, &h_);
    libusb_free_device_list(list, 1);
    if (rc != 0) {
        h_ = nullptr;
        return fail(std::string("libusb_open failed: ") + libusb_error_name(rc) +
                    (rc == LIBUSB_ERROR_ACCESS ? " (install the udev rule in packaging/ or run as root)" : ""));
    }
    vcIf_ = itf.vc;
    vsIf_ = itf.vs;
    bcdUVC_ = itf.bcd ? itf.bcd : 0x0100;
    streaming_ = itf.streaming;
    return claim();
}

bool UvcCamera::claim() {
    // If a kernel driver (uvcvideo) happens to be bound, let libusb detach it.
    libusb_set_auto_detach_kernel_driver(h_, 1);
    int rc = libusb_claim_interface(h_, vcIf_);
    if (rc != 0) return fail(std::string("claim VideoControl interface: ") + libusb_error_name(rc));
    rc = libusb_claim_interface(h_, vsIf_);
    if (rc != 0) return fail(std::string("claim VideoStreaming interface: ") + libusb_error_name(rc));
    libusb_set_interface_alt_setting(h_, vsIf_, 0);
    return true;
}

void UvcCamera::close() {
    stop();
    if (h_) {
        if (vsIf_ >= 0) libusb_release_interface(h_, vsIf_);
        if (vcIf_ >= 0) libusb_release_interface(h_, vcIf_);
        libusb_close(h_);
        h_ = nullptr;
    }
    vcIf_ = vsIf_ = -1;
}

bool UvcCamera::negotiate(const StreamRequest& req) {
    FormatDesc fmt;
    FrameDesc fr;
    if (!chooseFormat(streaming_, req, fmt, fr)) return fail("no supported uncompressed format matches the request");
    uint32_t interval = chooseInterval(fr, req.fps);

    // Some devices advertise one UVC version but accept a different probe
    // length, so fall back through the known sizes.
    const int preferred = probeControlSize(bcdUVC_);
    const int sizes[] = {preferred, 26, 34, 48};
    uint8_t probe[48];
    int len = 0;
    for (int sz : sizes) {
        memset(probe, 0, sizeof probe);
        wr16(probe + 0, 1);  // bmHint: keep dwFrameInterval fixed
        probe[2] = fmt.index;
        probe[3] = fr.index;
        wr32(probe + 4, interval);
        int rc = libusb_control_transfer(h_, 0x21, SET_CUR, VS_PROBE_CONTROL << 8, vsIf_, probe, sz, kCtrlTimeoutMs);
        if (rc == sz) {
            len = sz;
            break;
        }
    }
    if (!len) return fail("VS_PROBE SET_CUR rejected by device");

    int rc = libusb_control_transfer(h_, 0xA1, GET_CUR, VS_PROBE_CONTROL << 8, vsIf_, probe, len, kCtrlTimeoutMs);
    if (rc < 26) return fail(std::string("VS_PROBE GET_CUR failed: ") + (rc < 0 ? libusb_error_name(rc) : "short read"));

    rc = libusb_control_transfer(h_, 0x21, SET_CUR, VS_COMMIT_CONTROL << 8, vsIf_, probe, len, kCtrlTimeoutMs);
    if (rc != len) return fail(std::string("VS_COMMIT failed: ") + (rc < 0 ? libusb_error_name(rc) : "short write"));

    neg_ = NegotiatedStream{};
    neg_.format = fmt;
    neg_.frame = fr;
    neg_.interval = rd32(probe + 4) ? rd32(probe + 4) : interval;
    neg_.maxVideoFrameSize = rd32(probe + 18);
    neg_.maxPayloadSize = rd32(probe + 22);
    return true;
}

bool UvcCamera::chooseAltSetting() {
    libusb_device* dev = libusb_get_device(h_);
    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0) return fail("cannot read config descriptor");

    const libusb_interface* itf = nullptr;
    for (int i = 0; i < cfg->bNumInterfaces; ++i)
        if (cfg->interface[i].num_altsetting > 0 && cfg->interface[i].altsetting[0].bInterfaceNumber == vsIf_)
            itf = &cfg->interface[i];

    std::vector<std::pair<int, int>> isoAlts;  // (alt setting, bytes per microframe)
    std::vector<int> isoEps;
    int bulkEp = 0, bulkSize = 0;
    for (int a = 0; itf && a < itf->num_altsetting; ++a) {
        const libusb_interface_descriptor& alt = itf->altsetting[a];
        for (int e = 0; e < alt.bNumEndpoints; ++e) {
            const libusb_endpoint_descriptor& ep = alt.endpoint[e];
            if (!(ep.bEndpointAddress & LIBUSB_ENDPOINT_IN)) continue;
            int type = ep.bmAttributes & 3;
            if (type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
                isoAlts.emplace_back(alt.bAlternateSetting, isoPacketSize(ep));
                isoEps.push_back(ep.bEndpointAddress);
            } else if (type == LIBUSB_TRANSFER_TYPE_BULK && alt.bAlternateSetting == 0) {
                bulkEp = ep.bEndpointAddress;
                bulkSize = ep.wMaxPacketSize;
            }
        }
    }
    libusb_free_config_descriptor(cfg);

    int bestAlt = chooseIsoAlt(isoAlts, neg_.maxPayloadSize), bestSize = 0, bestEp = 0;
    for (size_t i = 0; i < isoAlts.size(); ++i)
        if (isoAlts[i].first == bestAlt) { bestSize = isoAlts[i].second; bestEp = isoEps[i]; }
    if (bestAlt >= 0) {
        neg_.bulk = false;
        neg_.altSetting = bestAlt;
        neg_.packetSize = bestSize;
        int rc = libusb_set_interface_alt_setting(h_, vsIf_, bestAlt);
        if (rc != 0) return fail(std::string("set alt setting: ") + libusb_error_name(rc));
        streaming_.endpointAddress = uint8_t(bestEp);
        return true;
    }
    if (bulkEp) {
        neg_.bulk = true;
        neg_.altSetting = 0;
        neg_.packetSize = bulkSize;
        streaming_.endpointAddress = uint8_t(bulkEp);
        return true;
    }
    return fail("VideoStreaming interface has no usable IN endpoint");
}

bool UvcCamera::submitTransfers() {
    transfers_.clear();
    if (neg_.bulk) {
        int size = neg_.maxPayloadSize ? int(neg_.maxPayloadSize)
                                       : (neg_.maxVideoFrameSize ? int(neg_.maxVideoFrameSize) : 65536);
        for (int i = 0; i < kBulkTransfers; ++i) {
            libusb_transfer* t = libusb_alloc_transfer(0);
            auto* buf = new uint8_t[size];
            libusb_fill_bulk_transfer(t, h_, streaming_.endpointAddress, buf, size, transferCallback, this, 0);
            transfers_.push_back(t);
        }
    } else {
        int size = neg_.packetSize * kIsoPacketsPerTransfer;
        for (int i = 0; i < kIsoTransfers; ++i) {
            libusb_transfer* t = libusb_alloc_transfer(kIsoPacketsPerTransfer);
            auto* buf = new uint8_t[size];
            libusb_fill_iso_transfer(t, h_, streaming_.endpointAddress, buf, size, kIsoPacketsPerTransfer,
                                     transferCallback, this, 0);
            libusb_set_iso_packet_lengths(t, neg_.packetSize);
            transfers_.push_back(t);
        }
    }
    for (auto* t : transfers_) {
        int rc = libusb_submit_transfer(t);
        if (rc != 0) return fail(std::string("submit transfer: ") + libusb_error_name(rc));
        ++activeTransfers_;
    }
    return true;
}

bool UvcCamera::start(const StreamRequest& req, FrameFn onFrame) {
    if (!h_) return fail("camera not open");
    stop();
    if (!negotiate(req)) return false;
    if (!chooseAltSetting()) return false;

    size_t expected = size_t(neg_.frame.width) * neg_.frame.height * bytesPerPixel(neg_.format.format);
    onFrame_ = std::move(onFrame);
    assembler_.reset(expected);
    assembler_.setCallback([this](const uint8_t* d, size_t n) {
        if (onFrame_) onFrame_(d, n);
    });

    running_ = true;
    if (!submitTransfers()) {
        running_ = false;
        // Let the event loop drain whatever did get submitted.
        thread_ = std::thread([this] { eventLoop(); });
        thread_.join();
        return false;
    }
    thread_ = std::thread([this] { eventLoop(); });
    return true;
}

void UvcCamera::eventLoop() {
    bool cancelled = false;
    while (running_ || activeTransfers_ > 0) {
        if (!running_ && !cancelled) {
            // Cancel from the event thread so no callback can resubmit after us.
            for (auto* t : transfers_) libusb_cancel_transfer(t);
            cancelled = true;
        }
        timeval tv{0, 100000};
        libusb_handle_events_timeout_completed(ctx_, &tv, nullptr);
    }
}

void UvcCamera::stop() {
    if (!thread_.joinable()) return;
    running_ = false;
    thread_.join();
    for (auto* t : transfers_) {
        delete[] t->buffer;
        libusb_free_transfer(t);
    }
    transfers_.clear();
    if (h_ && vsIf_ >= 0) libusb_set_interface_alt_setting(h_, vsIf_, 0);
}

void UvcCamera::transferCallback(libusb_transfer* t) {
    static_cast<UvcCamera*>(t->user_data)->onTransfer(t);
}

void UvcCamera::onTransfer(libusb_transfer* t) {
    if (t->status == LIBUSB_TRANSFER_COMPLETED) {
        if (t->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
            for (int i = 0; i < t->num_iso_packets; ++i) {
                const libusb_iso_packet_descriptor& pd = t->iso_packet_desc[i];
                if (pd.status != LIBUSB_TRANSFER_COMPLETED) {
                    if (pd.status != LIBUSB_TRANSFER_CANCELLED) ++transferErrors_;
                    continue;
                }
                assembler_.push(libusb_get_iso_packet_buffer_simple(t, i), pd.actual_length);
            }
        } else {
            assembler_.push(t->buffer, t->actual_length);
        }
    } else if (t->status != LIBUSB_TRANSFER_CANCELLED) {
        ++transferErrors_;
    }

    bool fatal = t->status == LIBUSB_TRANSFER_NO_DEVICE || t->status == LIBUSB_TRANSFER_CANCELLED;
    if (running_ && !fatal && libusb_submit_transfer(t) == 0) return;
    if (t->status == LIBUSB_TRANSFER_NO_DEVICE) err_ = "camera disconnected";
    --activeTransfers_;
}

std::string UvcCamera::stats() const {
    char s[200];
    snprintf(s, sizeof s, "frames ok=%llu dropped=%llu badHeaders=%llu usbErrors=%llu",
             (unsigned long long)assembler_.framesOk(), (unsigned long long)assembler_.framesDropped(),
             (unsigned long long)assembler_.badHeaders(), (unsigned long long)transferErrors_.load());
    return s;
}

}  // namespace uvc
