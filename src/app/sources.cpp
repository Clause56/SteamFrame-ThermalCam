#include "sources.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

void FrameMailbox::put(const uint8_t* d, size_t n, uvc::PixelFormat f, int w, int h) {
    {
        std::lock_guard<std::mutex> l(m_);
        slot_.data.assign(d, d + n);
        slot_.format = f;
        slot_.width = w;
        slot_.height = h;
        slot_.seq = ++seq_;
        fresh_ = true;
    }
    cv_.notify_one();
}

bool FrameMailbox::take(RawFrame& out, int timeoutMs) {
    std::unique_lock<std::mutex> l(m_);
    if (!cv_.wait_for(l, std::chrono::milliseconds(timeoutMs), [this] { return fresh_; })) return false;
    std::swap(out, slot_);
    fresh_ = false;
    return true;
}

// ---------------------------------------------------------------- UvcSource

bool UvcSource::start(FrameMailbox& box) {
    if (!cam_.open(vid_, pid_)) return false;
    return cam_.start(req_, [this, &box](const uint8_t* d, size_t n) {
        const auto& neg = cam_.negotiated();
        box.put(d, n, neg.format.format, neg.frame.width, neg.frame.height);
    });
}

std::string UvcSource::describe() const {
    const auto& n = cam_.negotiated();
    char s[256];
    snprintf(s, sizeof s, "UVC %x.%02x  %s %ux%u @ %.1f fps  %s alt=%d packet=%d maxPayload=%u",
             cam_.bcdUVC() >> 8, cam_.bcdUVC() & 0xff, uvc::pixelFormatName(n.format.format), n.frame.width,
             n.frame.height, n.interval ? 1e7 / n.interval : 0.0, n.bulk ? "bulk" : "isochronous", n.altSetting,
             n.packetSize, n.maxPayloadSize);
    return s;
}

// ---------------------------------------------------------------- SimSource

void SimSource::synthesize(double t, std::vector<uint8_t>& out) {
    const int W = kWidth, H = kHeight / 2;
    out.assign(size_t(kWidth) * kHeight * 2, 0);
    auto blob = [](float x, float y, float cx, float cy, float r) {
        float d2 = ((x - cx) * (x - cx) + (y - cy) * (y - cy)) / (r * r);
        return std::exp(-d2);
    };
    // A person's head drifting across a room, a hot mug, a cold window.
    float hx = W * (0.5f + 0.3f * std::sin(t * 0.6)), hy = H * 0.45f;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float c = 21.5f + 1.5f * y / H;  // room with slight gradient
            c += (34.5f - c) * std::min(1.f, 1.6f * blob(x, y, hx, hy, 28));            // face
            c += (33.0f - c) * std::min(1.f, 1.2f * blob(x, y, hx, hy + 70, 45));       // body
            c += (62.0f - c) * blob(x, y, W * 0.82f, H * 0.72f, 9);                      // mug
            if (x < W * 0.18f && y < H * 0.5f) c = 14.0f + 0.5f * std::sin(x * 0.2f);   // window
            if (x > W * 0.30f && x < W * 0.62f && y > H * 0.05f && y < H * 0.35f)       // bookshelf:
                c += ((x / 9 + y / 14) % 2 ? 0.4f : -0.3f);                              //  subtle texture
            if (x > W * 0.04f && x < W * 0.30f && y > H * 0.70f && y < H * 0.95f)       // radiator fins
                c = 36.f + 3.f * std::sin(x * 0.9f);
            c += 0.08f * std::sin(x * 12.9898f + y * 78.233f + float(t) * 50);          // sensor noise
            // Bottom half: temperature in 1/64 K, little endian.
            uint16_t raw = uint16_t(std::lround((c + 273.15f) * 64.f));
            size_t ti = (size_t(H + y) * W + x) * 2;
            out[ti] = raw & 0xff;
            out[ti + 1] = raw >> 8;
            // Top half: the camera's own 8-bit AGC image as YUYV luma, U/V = 128.
            size_t ii = (size_t(y) * W + x) * 2;
            out[ii] = uint8_t(std::clamp((c - 14.f) * 10.f, 0.f, 255.f));
            out[ii + 1] = 128;
        }
    }
}

bool SimSource::start(FrameMailbox& box) {
    stop();
    running_ = true;
    assembler_.reset(size_t(kWidth) * kHeight * 2);
    assembler_.setCallback([&box](const uint8_t* d, size_t n) {
        box.put(d, n, uvc::PixelFormat::YUYV, kWidth, kHeight);
    });
    thread_ = std::thread([this] {
        std::vector<uint8_t> frame, payload;
        const size_t kPayload = 3060;  // typical high-bandwidth iso packet (3 x 1020)
        uint8_t fid = 0;
        auto t0 = std::chrono::steady_clock::now();
        auto next = t0;
        while (running_) {
            double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            synthesize(t, frame);
            for (size_t off = 0; off < frame.size(); off += kPayload - 12) {
                size_t n = std::min(kPayload - 12, frame.size() - off);
                bool eof = off + n >= frame.size();
                // 12-byte header like real cameras (PTS + SCR present).
                payload.assign(12, 0);
                payload[0] = 12;
                payload[1] = uint8_t(0x80 | 0x0C | fid | (eof ? 0x02 : 0));
                payload.insert(payload.end(), frame.begin() + off, frame.begin() + off + n);
                assembler_.push(payload.data(), payload.size());
                assembler_.push(nullptr, 0);  // empty iso packets are common
            }
            fid ^= 1;
            next += std::chrono::microseconds(int64_t(1e6 / fps_));
            std::this_thread::sleep_until(next);
        }
    });
    return true;
}

void SimSource::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

std::string SimSource::stats() const {
    char s[128];
    snprintf(s, sizeof s, "frames ok=%llu dropped=%llu badHeaders=%llu", (unsigned long long)assembler_.framesOk(),
             (unsigned long long)assembler_.framesDropped(), (unsigned long long)assembler_.badHeaders());
    return s;
}
