// thermal-viewer: shows a cheap UVC thermal camera in the Steam Frame (or on
// a desktop) using a userland UVC driver, so the kernel's uvcvideo module
// is not required.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "../display/display.h"
#include "../thermal/renderer.h"
#include "../thermal/thermal_frame.h"
#include "../util/log.h"
#include "../util/png.h"
#include "sources.h"

namespace {

struct KnownCamera {
    uint16_t vid, pid;
    const char* name;
};
// Commonly reported IDs; any UVC device works, this only improves the listing.
const KnownCamera kKnown[] = {
    {0x0bda, 0x5840, "InfiRay P2 Pro (likely)"},
    {0x0bda, 0x5830, "Topdon TC001 / InfiRay P2"},
    {0x1e4e, 0x0100, "PureThermal / FLIR Lepton (likely)"},
};

const char* knownName(uint16_t vid, uint16_t pid) {
    for (const auto& k : kKnown)
        if (k.vid == vid && k.pid == pid) return k.name;
    return nullptr;
}

void usage() {
    printf(
        "thermal-viewer: userland UVC thermal camera viewer\n\n"
        "  --list                 list USB devices and the formats of any UVC cameras\n"
        "  --device VID:PID       camera to open (hex), default: first UVC device\n"
        "  --sim                  use the built-in simulated camera\n"
        "  --size WxH             preferred stream size   --fps N   frame rate\n"
        "  --format-index N / --frame-index N   pick descriptors explicitly\n"
        "  --mode M               auto|split|y16|y16-ck|grey (how to read pixels)\n"
        "  --display D            desktop|overlay|none (default: desktop)\n"
        "  --palette P            ironbow|white|black|rainbow|arctic\n"
        "  --gain G               equalize|camera|linear (default equalize: spreads colours\n"
        "                         over the temperatures in view and boosts fine detail;\n"
        "                         camera: colour the camera's own processed picture)\n"
        "  --detail X             edge/texture boost for --gain equalize (0..2, default 0.5)\n"
        "  --range LO:HI          fixed colour range instead of auto gain\n"
        "  --scale N              output upscale factor (default 3)\n"
        "  --flip h|v|hv  --rotate 0|90|180|270   match how the camera is mounted\n"
        "  --fahrenheit           show temperatures in F\n"
        "  --no-hud               hide crosshair, markers, text and colour bar\n"
        "  --overlay-distance M   --overlay-hfov DEG   --overlay-alpha A   --overlay-offset-y M\n"
        "  --dashboard            add a control panel to the SteamVR dashboard (experimental)\n"
        "  --overlay-buffering B  single|double (default single)\n"
        "  --log FILE             log file (default: thermal-viewer.log next to the program)\n"
        "  --frames N             stop after N frames      --snapshot FILE.png   save last frame\n"
        "  --config FILE          read options from FILE (default: thermal-viewer.conf next\n"
        "                         to the program; one or more options per line, # comments)\n"
        "  --register             add to SteamVR as an overlay app that starts with SteamVR\n"
        "  --unregister           remove it again\n"
        "\nDesktop keys: p palette, g gain mode, u C/F, h HUD, s snapshot, f fullscreen, q quit\n");
}

int listDevices() {
    auto devs = uvc::UvcCamera::enumerate();
    if (devs.empty()) {
        printf("No USB devices visible (is libusb able to read /dev/bus/usb?)\n");
        return 1;
    }
    for (const auto& d : devs) {
        const char* known = knownName(d.vid, d.pid);
        printf("Bus %03u Device %03u: ID %04x:%04x %s%s%s%s\n", d.bus, d.address, d.vid, d.pid,
               d.product.c_str(), known ? "  [" : "", known ? known : "", known ? "]" : "");
        if (d.isUvc) {
            printf("  UVC %x.%02x camera\n%s", d.bcdUVC >> 8, d.bcdUVC & 0xff, uvc::describe(d.streaming).c_str());
            if (!d.openError.empty())
                printf("  cannot open (%s): install packaging/60-thermal-camera.rules or run with sudo\n",
                       d.openError.c_str());
        }
    }
    return 0;
}

bool parseHexPair(const char* s, uint16_t& a, uint16_t& b) {
    unsigned x, y;
    if (sscanf(s, "%x:%x", &x, &y) != 2) return false;
    a = uint16_t(x);
    b = uint16_t(y);
    return true;
}

std::string timestampedName() {
    char buf[64];
    time_t t = time(nullptr);
    strftime(buf, sizeof buf, "thermal-%Y%m%d-%H%M%S.png", localtime(&t));
    return buf;
}

std::string exeDir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    std::string p(buf);
    size_t slash = p.rfind('/');
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

// Options from a config file, so the headset launch needs no command line.
std::vector<std::string> readConfig(const std::string& path, bool required) {
    std::vector<std::string> out;
    std::ifstream f(path);
    if (!f) {
        if (required) {
            fprintf(stderr, "cannot read config %s\n", path.c_str());
            exit(2);
        }
        return out;
    }
    std::string line;
    while (std::getline(f, line)) {
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::istringstream ss(line);
        std::string tok;
        while (ss >> tok) out.push_back(tok);
    }
    return out;
}

std::atomic<bool> gStop{false};
void onSignal(int) { gStop = true; }

}  // namespace

int main(int argc, char** argv) {
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    // Config file options first, so the command line can override them.
    std::string configPath = exeDir() + "/thermal-viewer.conf";
    bool configRequired = false;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--config") {
            configPath = argv[i + 1];
            configRequired = true;
        }
    std::vector<std::string> args = readConfig(configPath, configRequired);
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
    std::string logPath = exeDir() + "/thermal-viewer.log";

    uint16_t vid = 0, pid = 0;
    bool sim = false;
    uvc::StreamRequest req;
    thermal::DecodeMode mode = thermal::DecodeMode::Auto;
    thermal::RenderOptions ropt;
    OverlayConfig ocfg;
    std::string displayName = "desktop", snapshot;
    bool flipH = false, flipV = false;
    int rotate = 0;
    long maxFrames = 0;

    const int nargs = int(args.size());
    for (int i = 0; i < nargs; ++i) {
        std::string a = args[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= nargs) {
                fprintf(stderr, "%s needs a value\n", a.c_str());
                exit(2);
            }
            return args[++i].c_str();
        };
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--list") return listDevices();
        else if (a == "--sim") sim = true;
        else if (a == "--device") { if (!parseHexPair(next(), vid, pid)) { fprintf(stderr, "bad --device\n"); return 2; } }
        else if (a == "--size") { if (sscanf(next(), "%dx%d", &req.width, &req.height) != 2) { fprintf(stderr, "bad --size\n"); return 2; } }
        else if (a == "--fps") req.fps = atof(next());
        else if (a == "--format-index") req.formatIndex = atoi(next());
        else if (a == "--frame-index") req.frameIndex = atoi(next());
        else if (a == "--mode") { if (!thermal::parseDecodeMode(next(), mode)) { fprintf(stderr, "bad --mode\n"); return 2; } }
        else if (a == "--display") displayName = next();
        else if (a == "--palette") { if (!thermal::parsePalette(next(), ropt.palette)) { fprintf(stderr, "bad --palette\n"); return 2; } }
        else if (a == "--gain") { if (!thermal::parseGainMode(next(), ropt.gain)) { fprintf(stderr, "bad --gain\n"); return 2; } }
        else if (a == "--detail") ropt.detail = float(atof(next()));
        else if (a == "--no-dashboard") ocfg.dashboard = false;
        else if (a == "--dashboard") ocfg.dashboard = true;
        else if (a == "--overlay-buffering") {
            std::string b = next();
            if (b != "single" && b != "double") { fprintf(stderr, "bad --overlay-buffering\n"); return 2; }
            ocfg.doubleBuffer = b == "double";
        }
        else if (a == "--log") logPath = next();
        else if (a == "--config") next();
        else if (a == "--register") return registerWithSteamVR(true, exeDir());
        else if (a == "--unregister") return registerWithSteamVR(false, exeDir());
        else if (a == "--range") { if (sscanf(next(), "%f:%f", &ropt.lo, &ropt.hi) != 2) { fprintf(stderr, "bad --range\n"); return 2; } ropt.fixedRange = true; }
        else if (a == "--scale") ropt.scale = atoi(next());
        else if (a == "--flip") { std::string f = next(); flipH = f.find('h') != std::string::npos; flipV = f.find('v') != std::string::npos; }
        else if (a == "--rotate") rotate = atoi(next());
        else if (a == "--fahrenheit") ropt.fahrenheit = true;
        else if (a == "--no-hud") ropt.crosshair = ropt.markers = ropt.stats = ropt.colorBar = false;
        else if (a == "--overlay-distance") ocfg.distanceM = float(atof(next()));
        else if (a == "--overlay-hfov") ocfg.hfovDeg = float(atof(next()));
        else if (a == "--overlay-alpha") ocfg.alpha = float(atof(next()));
        else if (a == "--overlay-offset-y") ocfg.offsetYM = float(atof(next()));
        else if (a == "--frames") maxFrames = atol(next());
        else if (a == "--snapshot") snapshot = next();
        else { fprintf(stderr, "unknown option %s\n\n", a.c_str()); usage(); return 2; }
    }

    logOpen(logPath);
    {
        std::string all;
        for (const auto& a : args) all += " " + a;
        logMsg("thermal-viewer starting:%s", all.c_str());
    }

    std::unique_ptr<Display> display;
    if (displayName == "desktop") display = makeDesktopDisplay();
    else if (displayName == "overlay") display = makeOverlayDisplay(ocfg);
    else if (displayName != "none") { fprintf(stderr, "unknown display %s\n", displayName.c_str()); return 2; }
    if (displayName != "none") {
        if (!display) {
            fprintf(stderr, "this build has no '%s' display support\n", displayName.c_str());
            return 1;
        }
        std::string err;
        logMsg("starting %s display", display->name());
        if (!display->init(err)) {
            logMsg("%s display: %s", display->name(), err.c_str());
            return 1;
        }
    }

    std::unique_ptr<FrameSource> src;
    if (sim) src = std::make_unique<SimSource>();
    else src = std::make_unique<UvcSource>(vid, pid, req);

    FrameMailbox box;
    if (!src->start(box)) {
        logMsg("camera: %s", src->error().c_str());
        fprintf(stderr, "run with --list to see what libusb can find, or --sim to test without a camera\n");
        return 1;
    }
    logMsg("streaming: %s", src->describe().c_str());

    thermal::Renderer renderer;
    thermal::ThermalFrame tf;
    thermal::RgbaImage img;
    RawFrame raw;
    long frames = 0;
    bool reportedMode = false;
    auto lastFrame = std::chrono::steady_clock::now();
    auto lastLog = lastFrame;
    bool warnedStall = false;
    UiState ui;

    while (!gStop) {
        UiState cur{thermal::paletteName(ropt.palette), thermal::gainModeName(ropt.gain), ropt.fahrenheit};
        if (display && !(cur == ui)) {
            display->setUiState(cur);
            ui = cur;
        }
        DisplayEvents ev = display ? display->poll() : DisplayEvents{};
        if (ev.quit) break;
        if (ev.nextPalette) ropt.palette = thermal::Palette((int(ropt.palette) + 1) % int(thermal::Palette::Count));
        if (ev.nextGain) ropt.gain = thermal::GainMode((int(ropt.gain) + 1) % int(thermal::GainMode::Count));
        if (ev.toggleUnits) ropt.fahrenheit = !ropt.fahrenheit;
        if (ev.toggleHud) ropt.crosshair = ropt.markers = ropt.stats = ropt.colorBar = !ropt.stats;
        if (ev.snapshot && !img.px.empty()) {
            std::string name = timestampedName();
            printf("%s %s\n", writePng(name, img.px.data(), img.width, img.height) ? "saved" : "failed to save", name.c_str());
        }

        auto now = std::chrono::steady_clock::now();
        if (!box.take(raw, 10)) {  // short wait keeps overlay events flowing
            if (!warnedStall && now - lastFrame > std::chrono::seconds(3)) {
                logMsg("no frames for 3 s (%s) %s", src->stats().c_str(), src->error().c_str());
                warnedStall = true;
            }
            continue;
        }
        lastFrame = now;
        warnedStall = false;

        if (!thermal::decode(raw.data.data(), raw.data.size(), raw.format, raw.width, raw.height, mode, tf)) {
            fprintf(stderr, "cannot decode %s %dx%d as %s\n", uvc::pixelFormatName(raw.format), raw.width, raw.height,
                    thermal::decodeModeName(mode));
            continue;
        }
        if (!reportedMode) {
            auto m = thermal::resolveMode(mode, raw.format, raw.width, raw.height);
            printf("decoding as '%s' -> %dx%d %s\n", thermal::decodeModeName(m), tf.width, tf.height,
                   tf.units == thermal::Units::Celsius ? "temperatures" : "raw intensity");
            reportedMode = true;
        }
        tf.seq = raw.seq;
        thermal::transform(tf, flipH, flipV, rotate);
        renderer.render(tf, ropt, img);
        if (display) display->present(img);
        ++frames;

        if (now - lastLog > std::chrono::seconds(5)) {
            const auto& s = renderer.lastStats();
            logMsg("frame %ld  min %.1f  max %.1f  centre %.1f  | %s", frames, s.min, s.max, s.center,
                   src->stats().c_str());
            lastLog = now;
        }
        if (maxFrames && frames >= maxFrames) break;
    }

    src->stop();
    if (!snapshot.empty() && !img.px.empty()) {
        bool ok = writePng(snapshot, img.px.data(), img.width, img.height);
        printf("%s %s\n", ok ? "saved" : "failed to save", snapshot.c_str());
        if (!ok) return 1;
    }
    logMsg("done: %ld frames  %s%s", frames, src->stats().c_str(), gStop ? " (stopped by signal)" : "");
    return 0;
}
