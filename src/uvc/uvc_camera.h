// Userland UVC camera driver built on libusb.
//
// Replaces the kernel's uvcvideo module: it finds the VideoControl and
// VideoStreaming interfaces, negotiates a format with the probe/commit
// controls, selects an alternate setting with enough isochronous bandwidth
// (or uses the bulk endpoint), and reassembles frames from the payloads.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "frame_assembler.h"
#include "uvc_descriptors.h"

struct libusb_context;
struct libusb_device_handle;
struct libusb_transfer;

namespace uvc {

struct DeviceSummary {
    uint16_t vid = 0, pid = 0;
    uint8_t bus = 0, address = 0;
    std::string product;   // may be empty if the string can't be read (permissions)
    bool isUvc = false;
    uint16_t bcdUVC = 0;
    StreamingDesc streaming;
    std::string openError;  // why string/descriptor reads failed, if they did
};

struct StreamRequest {
    int formatIndex = 0;   // 0 = choose automatically
    int frameIndex = 0;    // 0 = choose automatically
    int width = 0, height = 0;  // preferred size if indices are 0
    double fps = 0;        // 0 = device default
};

struct NegotiatedStream {
    FormatDesc format;
    FrameDesc frame;
    uint32_t interval = 0;
    uint32_t maxVideoFrameSize = 0;
    uint32_t maxPayloadSize = 0;
    bool bulk = false;
    int altSetting = 0;
    int packetSize = 0;
};

// Stream selection, exposed for tests.
bool chooseFormat(const StreamingDesc& s, const StreamRequest& req, FormatDesc& fmt, FrameDesc& frame);
// alts = (alternate setting, isochronous bytes per microframe); returns the
// smallest alt that fits maxPayload, else the widest, else -1.
int chooseIsoAlt(const std::vector<std::pair<int, int>>& alts, uint32_t maxPayload);

class UvcCamera {
public:
    using FrameFn = std::function<void(const uint8_t* data, size_t len)>;

    UvcCamera();
    ~UvcCamera();
    UvcCamera(const UvcCamera&) = delete;
    UvcCamera& operator=(const UvcCamera&) = delete;

    static std::vector<DeviceSummary> enumerate();

    // vid/pid of 0 = first UVC device found.
    bool open(uint16_t vid, uint16_t pid);
    void close();

    bool start(const StreamRequest& req, FrameFn onFrame);
    void stop();

    const StreamingDesc& streamingDesc() const { return streaming_; }
    const NegotiatedStream& negotiated() const { return neg_; }
    uint16_t bcdUVC() const { return bcdUVC_; }
    const std::string& lastError() const { return err_; }
    std::string stats() const;

private:
    bool claim();
    bool negotiate(const StreamRequest& req);
    bool chooseAltSetting();
    bool submitTransfers();
    void eventLoop();
    void onTransfer(libusb_transfer* t);
    static void transferCallback(libusb_transfer* t);
    bool fail(const std::string& msg);

    libusb_context* ctx_ = nullptr;
    libusb_device_handle* h_ = nullptr;
    int vcIf_ = -1, vsIf_ = -1;
    uint16_t bcdUVC_ = 0;
    StreamingDesc streaming_;
    NegotiatedStream neg_;
    FrameAssembler assembler_;
    FrameFn onFrame_;
    std::vector<libusb_transfer*> transfers_;
    std::atomic<int> activeTransfers_{0};
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::string err_;
    std::atomic<uint64_t> transferErrors_{0};
};

}  // namespace uvc
