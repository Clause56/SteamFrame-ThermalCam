// SteamVR overlay: a panel locked to the headset, sized so the thermal
// image lines up with the real world when the camera is mounted on the
// front of the headset. Overlays draw on top of whatever else is running
// (SteamVR Home, passthrough, games), so no scene app is needed.
//
// Also adds a control panel to the SteamVR dashboard (show/hide, palette,
// gain, opacity, size, units, quit) so no keyboard is needed in the headset.
#include "display.h"

#include <cstdio>

#ifdef HAVE_OPENVR
#include <openvr.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr const char* kAppKey = "thermal.viewer";

// SteamVR doesn't always file an added manifest under its app_key: some
// versions register it under a generated key (e.g. "system.generated.*").
// Find the entry that launches our binary, trying the declared key first.
std::string findAppKey(const std::string& binaryPath) {
    vr::IVRApplications* apps = vr::VRApplications();
    if (!apps) return {};
    if (apps->IsApplicationInstalled(kAppKey)) return kAppKey;
    char key[vr::k_unMaxApplicationKeyLength], path[4096];
    for (uint32_t i = 0, n = apps->GetApplicationCount(); i < n; ++i) {
        if (apps->GetApplicationKeyByIndex(i, key, sizeof key) != vr::VRApplicationError_None) continue;
        vr::EVRApplicationError err = vr::VRApplicationError_None;
        apps->GetApplicationPropertyString(key, vr::VRApplicationProperty_BinaryPath_String, path, sizeof path, &err);
        if (err == vr::VRApplicationError_None && binaryPath == path) return key;
        if (std::string(key).find("thermal.viewer") != std::string::npos) return key;
    }
    return {};
}

std::string selfPath() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return {};
    buf[n] = 0;
    return buf;
}

constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24);
}

enum ButtonId { BtnToggle, BtnPalette, BtnGain, BtnOpacityDown, BtnOpacityUp, BtnSizeDown, BtnSizeUp, BtnUnits, BtnQuit };

struct Button {
    int x, y, w, h;
    ButtonId id;
    std::string label;
};

class OverlayDisplay : public Display {
public:
    explicit OverlayDisplay(const OverlayConfig& c) : cfg_(c), alpha_(c.alpha) {}
    ~OverlayDisplay() override {
        if (vr::VROverlay())
            for (auto h : {ov_[0], ov_[1], dash_, thumb_})
                if (h != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->DestroyOverlay(h);
        if (inited_) vr::VR_Shutdown();
    }

    bool init(std::string& err) override {
        vr::EVRInitError e = vr::VRInitError_None;
        vr::VR_Init(&e, vr::VRApplication_Overlay);
        if (e != vr::VRInitError_None) {
            err = std::string("SteamVR init failed: ") + vr::VR_GetVRInitErrorAsEnglishDescription(e);
            return false;
        }
        inited_ = true;
        vr::IVROverlay* ov = vr::VROverlay();
        if (!ov) {
            err = "IVROverlay unavailable";
            return false;
        }
        // Let SteamVR associate this process with the registered app, if any.
        std::string key = findAppKey(selfPath());
        if (!key.empty()) vr::VRApplications()->IdentifyApplication(uint32_t(getpid()), key.c_str());

        // Two overlays used as a double buffer: each new frame is uploaded to
        // the hidden one and only shown once SteamVR has finished loading it.
        // Re-uploading a single visible overlay makes it blank out while the
        // new image loads, which is what caused the strobing.
        const char* keys[2] = {"thermal.viewer.a", "thermal.viewer.b"};
        for (int i = 0; i < 2; ++i) {
            vr::EVROverlayError oe = ov->CreateOverlay(keys[i], "Thermal Camera", &ov_[i]);
            if (oe != vr::VROverlayError_None) {
                err = std::string("CreateOverlay: ") + ov->GetOverlayErrorNameFromEnum(oe);
                return false;
            }
            ov->SetOverlayAlpha(ov_[i], alpha_);
            ov->SetOverlaySortOrder(ov_[i], 100);
        }

        if (cfg_.dashboard) {
            vr::EVROverlayError oe = ov->CreateDashboardOverlay("thermal.viewer.dashboard", "Thermal Camera", &dash_, &thumb_);
            if (oe == vr::VROverlayError_None) {
                ov->SetOverlayWidthInMeters(dash_, 1.6f);
                ov->SetOverlayInputMethod(dash_, vr::VROverlayInputMethod_Mouse);
                vr::HmdVector2_t scale = {float(kDashW), float(kDashH)};
                ov->SetOverlayMouseScale(dash_, &scale);
                drawThumbnail();
                drawDashboard();
            } else {
                fprintf(stderr, "dashboard panel unavailable: %s\n", ov->GetOverlayErrorNameFromEnum(oe));
                dash_ = thumb_ = vr::k_ulOverlayHandleInvalid;
            }
        }
        return true;
    }

    void present(const thermal::RgbaImage& img) override {
        if (!visible_) return;
        if (img.width != lastW_ || img.height != lastH_ || img.imageW != lastImgW_) {
            lastW_ = img.width;
            lastH_ = img.height;
            lastImgW_ = img.imageW;
            lastImgH_ = img.imageH;
            lastImgX_ = img.imageX;
            lastImgY_ = img.imageY;
            place();
        }
        // No load confirmation arrived within a frame: swap anyway, the
        // upload of a small image finishes well within one frame period.
        if (pending_) swap();
        int back = 1 - front_;
        vr::EVROverlayError e =
            vr::VROverlay()->SetOverlayRaw(ov_[back], const_cast<uint8_t*>(img.px.data()), img.width, img.height, 4);
        if (e != vr::VROverlayError_None) {
            if (!warnedUpload_)
                fprintf(stderr, "SetOverlayRaw: %s\n", vr::VROverlay()->GetOverlayErrorNameFromEnum(e));
            warnedUpload_ = true;
            return;
        }
        pending_ = true;
    }

    DisplayEvents poll() override {
        DisplayEvents ev;
        vr::VREvent_t e;
        for (int i = 0; i < 2; ++i)
            while (vr::VROverlay()->PollNextOverlayEvent(ov_[i], &e, sizeof e)) {
                if (e.eventType == vr::VREvent_ImageLoaded && pending_ && i == 1 - front_) swap();
                if (e.eventType == vr::VREvent_Quit) ev.quit = true;
            }
        if (dash_ != vr::k_ulOverlayHandleInvalid)
            while (vr::VROverlay()->PollNextOverlayEvent(dash_, &e, sizeof e)) {
                if (e.eventType == vr::VREvent_MouseButtonUp) click(e.data.mouse.x, kDashH - e.data.mouse.y, ev);
                if (e.eventType == vr::VREvent_Quit) ev.quit = true;
            }
        vr::VREvent_t se;
        while (vr::VRSystem() && vr::VRSystem()->PollNextEvent(&se, sizeof se))
            if (se.eventType == vr::VREvent_Quit) {
                vr::VRSystem()->AcknowledgeQuit_Exiting();
                ev.quit = true;
            }
        return ev;
    }

    void setUiState(const UiState& s) override {
        ui_ = s;
        drawDashboard();
    }

    const char* name() const override { return "steamvr-overlay"; }

private:
    static constexpr int kDashW = 640, kDashH = 420;

    void swap() {
        int back = 1 - front_;
        if (visible_) {
            vr::VROverlay()->ShowOverlay(ov_[back]);  // show new before hiding old: no gap
            vr::VROverlay()->HideOverlay(ov_[front_]);
        }
        front_ = back;
        pending_ = false;
    }

    // Sizes and positions both panels so the thermal image itself (not the HUD
    // border around it) subtends the camera's field of view, centred on the
    // line of sight. That keeps it aligned with the real world at size 100%.
    void place() {
        if (!lastW_) return;
        int imgW = lastImgW_ ? lastImgW_ : lastW_;
        int imgH = lastImgH_ ? lastImgH_ : lastH_;
        float imageWidthM = 2.f * cfg_.distanceM * std::tan(cfg_.hfovDeg * 3.14159265f / 360.f) * sizeScale_;
        float mpp = imageWidthM / imgW;  // metres per pixel
        float dx = ((lastW_ / 2.f) - (lastImgX_ + imgW / 2.f)) * mpp;
        float dy = -((lastH_ / 2.f) - (lastImgY_ + imgH / 2.f)) * mpp;
        vr::HmdMatrix34_t m = {{{1, 0, 0, dx}, {0, 1, 0, cfg_.offsetYM + dy}, {0, 0, 1, -cfg_.distanceM}}};
        for (auto h : ov_) {
            vr::VROverlay()->SetOverlayWidthInMeters(h, mpp * lastW_);
            vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(h, vr::k_unTrackedDeviceIndex_Hmd, &m);
        }
    }

    void click(float x, float y, DisplayEvents& ev) {
        for (const auto& b : buttons_) {
            if (x < b.x || y < b.y || x >= b.x + b.w || y >= b.y + b.h) continue;
            switch (b.id) {
                case BtnToggle:
                    visible_ = !visible_;
                    for (auto h : ov_) vr::VROverlay()->HideOverlay(h);
                    pending_ = false;  // the next frame re-shows the view
                    break;
                case BtnPalette: ev.nextPalette = true; break;
                case BtnGain: ev.nextGain = true; break;
                case BtnUnits: ev.toggleUnits = true; break;
                case BtnQuit: ev.quit = true; break;
                case BtnOpacityDown:
                case BtnOpacityUp:
                    alpha_ = std::clamp(alpha_ + (b.id == BtnOpacityUp ? 0.1f : -0.1f), 0.2f, 1.f);
                    for (auto h : ov_) vr::VROverlay()->SetOverlayAlpha(h, alpha_);
                    break;
                case BtnSizeDown:
                case BtnSizeUp:
                    sizeScale_ = std::clamp(sizeScale_ * (b.id == BtnSizeUp ? 1.1f : 1.f / 1.1f), 0.3f, 3.f);
                    if (std::fabs(sizeScale_ - 1.f) < 0.02f) sizeScale_ = 1.f;
                    place();
                    break;
            }
            drawDashboard();
            return;
        }
    }

    void drawDashboard() {
        if (dash_ == vr::k_ulOverlayHandleInvalid) return;
        auto upper = [](std::string s) {
            for (auto& c : s) c = char(toupper(c));
            return s;
        };
        char opacity[32], size[32];
        snprintf(opacity, sizeof opacity, "OPACITY %d%%", int(std::lround(alpha_ * 100)));
        snprintf(size, sizeof size, "SIZE %d%%", int(std::lround(sizeScale_ * 100)));
        const int m = 20, gap = 14, rowH = 56, colW = (kDashW - 2 * m - gap) / 2, small = 80;
        int y = 70;
        buttons_.clear();
        buttons_.push_back({m, y, kDashW - 2 * m, rowH, BtnToggle, visible_ ? "HIDE CAMERA VIEW" : "SHOW CAMERA VIEW"});
        y += rowH + gap;
        buttons_.push_back({m, y, colW, rowH, BtnPalette, "PALETTE " + upper(ui_.palette)});
        buttons_.push_back({m + colW + gap, y, colW, rowH, BtnGain, "DETAIL " + upper(ui_.gain)});
        y += rowH + gap;
        buttons_.push_back({m, y, small, rowH, BtnOpacityDown, "-"});
        buttons_.push_back({kDashW - m - small, y, small, rowH, BtnOpacityUp, "+"});
        int labelOpacityY = y;
        y += rowH + gap;
        buttons_.push_back({m, y, small, rowH, BtnSizeDown, "-"});
        buttons_.push_back({kDashW - m - small, y, small, rowH, BtnSizeUp, "+"});
        int labelSizeY = y;
        y += rowH + gap;
        buttons_.push_back({m, y, colW, rowH, BtnUnits, ui_.fahrenheit ? "UNITS *F" : "UNITS *C"});
        buttons_.push_back({m + colW + gap, y, colW, rowH, BtnQuit, "QUIT"});

        thermal::RgbaImage img;
        img.width = kDashW;
        img.height = kDashH;
        img.px.assign(size_t(kDashW) * kDashH * 4, 0);
        thermal::fillRect(img, 0, 0, kDashW, kDashH, rgba(24, 26, 32));
        const std::string title = "THERMAL CAMERA";
        thermal::drawText(img, (kDashW - thermal::textWidth(title, 4)) / 2, 20, title, 4, rgba(255, 170, 60));
        for (const auto& b : buttons_) {
            uint32_t bg = b.id == BtnQuit ? rgba(120, 40, 40) : rgba(52, 58, 72);
            thermal::fillRect(img, b.x, b.y, b.w, b.h, bg);
            int ts = 3;
            thermal::drawText(img, b.x + (b.w - thermal::textWidth(b.label, ts)) / 2, b.y + (b.h - 7 * ts) / 2, b.label,
                              ts, rgba(255, 255, 255));
        }
        auto centred = [&](const char* s, int yy) {
            thermal::drawText(img, (kDashW - thermal::textWidth(s, 3)) / 2, yy + (rowH - 21) / 2, s, 3, rgba(220, 220, 220));
        };
        centred(opacity, labelOpacityY);
        centred(size, labelSizeY);
        vr::VROverlay()->SetOverlayRaw(dash_, img.px.data(), img.width, img.height, 4);
    }

    void drawThumbnail() {
        if (thumb_ == vr::k_ulOverlayHandleInvalid) return;
        const int n = 128;
        thermal::RgbaImage img;
        img.width = img.height = n;
        img.px.resize(size_t(n) * n * 4);
        for (int y = 0; y < n; ++y) {
            float t = 1.f - float(y) / (n - 1);  // warm at the top
            uint32_t c = rgba(uint8_t(std::min(255.f, 60 + 400 * t)), uint8_t(std::max(0.f, 255 * (t - 0.4f) * 1.6f)),
                              uint8_t(std::max(0.f, 150 * (1 - 2 * t))));
            thermal::fillRect(img, 0, y, n, 1, c);
        }
        thermal::drawText(img, (n - thermal::textWidth("IR", 8)) / 2, (n - 56) / 2, "IR", 8, rgba(255, 255, 255));
        vr::VROverlay()->SetOverlayRaw(thumb_, img.px.data(), n, n, 4);
    }

    OverlayConfig cfg_;
    vr::VROverlayHandle_t ov_[2] = {vr::k_ulOverlayHandleInvalid, vr::k_ulOverlayHandleInvalid};
    vr::VROverlayHandle_t dash_ = vr::k_ulOverlayHandleInvalid, thumb_ = vr::k_ulOverlayHandleInvalid;
    int front_ = 0;
    bool pending_ = false;
    bool visible_ = true;
    bool inited_ = false;
    bool warnedUpload_ = false;
    float alpha_;
    float sizeScale_ = 1.f;
    int lastW_ = 0, lastH_ = 0, lastImgW_ = 0, lastImgH_ = 0, lastImgX_ = 0, lastImgY_ = 0;
    UiState ui_;
    std::vector<Button> buttons_;
};

}  // namespace

std::unique_ptr<Display> makeOverlayDisplay(const OverlayConfig& cfg) { return std::make_unique<OverlayDisplay>(cfg); }

int registerWithSteamVR(bool enable, const std::string& exeDir) {
    const std::string manifest = exeDir + "/thermal-viewer.vrmanifest";
    if (enable) {
        FILE* f = fopen(manifest.c_str(), "w");
        if (!f) {
            fprintf(stderr, "cannot write %s\n", manifest.c_str());
            return 1;
        }
        fprintf(f,
                "{\n"
                "  \"source\": \"builtin\",\n"
                "  \"applications\": [{\n"
                "    \"app_key\": \"%s\",\n"
                "    \"launch_type\": \"binary\",\n"
                "    \"binary_path_linux\": \"%s/thermal-viewer\",\n"
                "    \"binary_path_linuxarm64\": \"%s/thermal-viewer\",\n"
                "    \"arguments\": \"--display overlay\",\n"
                "    \"is_dashboard_overlay\": true,\n"
                "    \"strings\": { \"en_us\": { \"name\": \"Thermal Camera\",\n"
                "      \"description\": \"Head-locked view of a USB thermal camera\" } }\n"
                "  }]\n"
                "}\n",
                kAppKey, exeDir.c_str(), exeDir.c_str());
        fclose(f);
    }
    vr::EVRInitError e = vr::VRInitError_None;
    vr::VR_Init(&e, vr::VRApplication_Utility);
    if (e != vr::VRInitError_None) {
        fprintf(stderr, "SteamVR must be running: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(e));
        return 1;
    }
    vr::IVRApplications* apps = vr::VRApplications();
    int rc = 0;
    const std::string binary = exeDir + "/thermal-viewer";
    if (enable) {
        vr::EVRApplicationError ae = apps->AddApplicationManifest(manifest.c_str(), false);
        if (ae != vr::VRApplicationError_None) {
            fprintf(stderr, "SteamVR rejected %s: %s\n", manifest.c_str(), apps->GetApplicationsErrorNameFromEnum(ae));
            vr::VR_Shutdown();
            return 1;
        }
        // The manifest is processed asynchronously; give SteamVR a moment.
        std::string key;
        for (int i = 0; i < 50 && key.empty(); ++i) {
            key = findAppKey(binary);
            if (key.empty()) usleep(100000);
        }
        if (key.empty()) {
            fprintf(stderr,
                    "SteamVR accepted the manifest but hasn't listed the app yet.\n"
                    "Restart SteamVR and run --register again. Manifest: %s\n",
                    manifest.c_str());
            rc = 1;
        } else if ((ae = apps->SetApplicationAutoLaunch(key.c_str(), true)) != vr::VRApplicationError_None) {
            fprintf(stderr,
                    "Added to SteamVR as '%s', but auto-launch failed (%s).\n"
                    "Turn it on by hand: SteamVR Settings > Startup/Shutdown > Choose Startup Overlay Apps.\n",
                    key.c_str(), apps->GetApplicationsErrorNameFromEnum(ae));
            rc = 1;
        } else {
            printf("Registered as '%s'. Thermal Camera now starts with SteamVR (toggle it under\n"
                   "SteamVR Settings > Startup/Shutdown > Choose Startup Overlay Apps).\n",
                   key.c_str());
        }
    } else {
        std::string key = findAppKey(binary);
        if (!key.empty()) apps->SetApplicationAutoLaunch(key.c_str(), false);
        apps->RemoveApplicationManifest(manifest.c_str());
        printf("Removed from SteamVR.\n");
    }
    vr::VR_Shutdown();
    return rc;
}
#else
std::unique_ptr<Display> makeOverlayDisplay(const OverlayConfig&) { return nullptr; }
int registerWithSteamVR(bool, const std::string&) {
    fprintf(stderr, "this build has no SteamVR support\n");
    return 1;
}
#endif
