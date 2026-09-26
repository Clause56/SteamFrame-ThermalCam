#include "frame_assembler.h"

namespace uvc {

// bmHeaderInfo bits (UVC 1.5, section 2.4.3.3)
constexpr uint8_t HDR_FID = 0x01;
constexpr uint8_t HDR_EOF = 0x02;
constexpr uint8_t HDR_ERR = 0x40;

void FrameAssembler::reset(size_t expectedFrameBytes) {
    expected_ = expectedFrameBytes;
    buf_.clear();
    buf_.reserve(expected_ ? expected_ + 4096 : 1 << 20);
    lastFid_ = -1;
    error_ = false;
}

void FrameAssembler::push(const uint8_t* p, size_t len) {
    if (len == 0) return;  // empty isochronous packet, normal between frames
    size_t hl = p[0];
    if (len < 2 || hl < 2 || hl > len) {
        ++badHeaders_;
        return;
    }
    uint8_t info = p[1];
    int fid = info & HDR_FID;

    // A toggled frame ID means a new frame started; flush the previous one
    // (covers cameras that never set EOF).
    if (lastFid_ != -1 && fid != lastFid_ && !buf_.empty()) finish();
    lastFid_ = fid;

    if (info & HDR_ERR) error_ = true;

    size_t n = len - hl;
    if (n) {
        // Guard against runaway frames if EOF/FID are both lost.
        if (expected_ && buf_.size() + n > expected_ * 2) {
            error_ = true;
            buf_.clear();
        }
        buf_.insert(buf_.end(), p + hl, p + len);
    }

    if (info & HDR_EOF) finish();
}

void FrameAssembler::finish() {
    bool ok = !error_ && !buf_.empty() && (expected_ == 0 || buf_.size() >= expected_);
    if (ok) {
        ++framesOk_;
        if (onFrame_) onFrame_(buf_.data(), expected_ ? expected_ : buf_.size());
    } else if (!buf_.empty() || error_) {
        ++framesDropped_;
    }
    buf_.clear();
    error_ = false;
}

}  // namespace uvc
