// Frame sources: the real USB camera (userland UVC) and a simulator that
// produces byte-identical UVC payloads so the whole pipeline can be tested
// without hardware.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../uvc/frame_assembler.h"
#include "../uvc/uvc_camera.h"

struct RawFrame {
    std::vector<uint8_t> data;
    uvc::PixelFormat format = uvc::PixelFormat::Unknown;
    int width = 0, height = 0;
    uint64_t seq = 0;
};

// Single-slot mailbox: the capture thread overwrites, the display takes the
// newest frame. Old frames are dropped rather than queued, which keeps
// latency minimal in the headset.
class FrameMailbox {
public:
    void put(const uint8_t* d, size_t n, uvc::PixelFormat f, int w, int h);
    bool take(RawFrame& out, int timeoutMs);
    uint64_t produced() const { return seq_; }

private:
    std::mutex m_;
    std::condition_variable cv_;
    RawFrame slot_;
    bool fresh_ = false;
    uint64_t seq_ = 0;
};

class FrameSource {
public:
    virtual ~FrameSource() = default;
    virtual bool start(FrameMailbox& box) = 0;
    virtual void stop() = 0;
    virtual std::string describe() const = 0;
    virtual std::string stats() const { return {}; }
    virtual std::string error() const { return {}; }
};

class UvcSource : public FrameSource {
public:
    UvcSource(uint16_t vid, uint16_t pid, uvc::StreamRequest req) : vid_(vid), pid_(pid), req_(req) {}
    bool start(FrameMailbox& box) override;
    void stop() override { cam_.close(); }
    std::string describe() const override;
    std::string stats() const override { return cam_.stats(); }
    std::string error() const override { return cam_.lastError(); }

private:
    uint16_t vid_, pid_;
    uvc::StreamRequest req_;
    uvc::UvcCamera cam_;
};

// Simulates an InfiRay/Topdon-style camera: 256x384 YUYV where the bottom
// half carries temperatures in 1/64 Kelvin. Frames are chopped into UVC
// payloads (with FID/EOF headers) and pushed through a real FrameAssembler.
class SimSource : public FrameSource {
public:
    explicit SimSource(double fps = 25) : fps_(fps) {}
    ~SimSource() override { stop(); }
    bool start(FrameMailbox& box) override;
    void stop() override;
    std::string describe() const override { return "simulated InfiRay-style camera 256x384 YUYV (split image + temperature)"; }
    std::string stats() const override;

    static constexpr int kWidth = 256, kHeight = 384;
    // Builds one raw frame for time t (seconds). Exposed for tests.
    static void synthesize(double t, std::vector<uint8_t>& out);

private:
    double fps_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    uvc::FrameAssembler assembler_;
};
