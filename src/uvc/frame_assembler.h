// Reassembles UVC payloads (one per isochronous packet or bulk transfer)
// into complete video frames. Pure logic, no libusb dependency.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace uvc {

class FrameAssembler {
public:
    using FrameFn = std::function<void(const uint8_t* data, size_t len)>;

    explicit FrameAssembler(size_t expectedFrameBytes = 0) { reset(expectedFrameBytes); }

    void reset(size_t expectedFrameBytes);
    void setCallback(FrameFn fn) { onFrame_ = std::move(fn); }

    // Feed one payload: UVC payload header followed by image data.
    void push(const uint8_t* payload, size_t len);

    uint64_t framesOk() const { return framesOk_; }
    uint64_t framesDropped() const { return framesDropped_; }
    uint64_t badHeaders() const { return badHeaders_; }

private:
    void finish();

    FrameFn onFrame_;
    std::vector<uint8_t> buf_;
    size_t expected_ = 0;
    int lastFid_ = -1;
    bool error_ = false;
    uint64_t framesOk_ = 0, framesDropped_ = 0, badHeaders_ = 0;
};

}  // namespace uvc
