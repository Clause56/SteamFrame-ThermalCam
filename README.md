# thermal-viewer

Shows a cheap USB (UVC) thermal camera inside the Valve Steam Frame. The
Frame's kernel lacks the `uvcvideo` driver, so this program has its own
**userland UVC driver** on top of libusb: no kernel module is needed.

```
USB camera ──libusb──> UVC driver ──> frame assembler ──> decoder ──> renderer ──> display
             (src/uvc: descriptors, probe/commit,        (src/thermal:        desktop window
              isochronous or bulk streaming)              °C, palettes, HUD)   SteamVR overlay
                                                                               PNG snapshot
```

## Supported cameras

Any UVC camera that streams uncompressed YUYV, Y8 or Y16. Decoding is picked
automatically and can be forced with `--mode`:

| mode     | cameras | what it reads |
|----------|---------|---------------|
| `split`  | Topdon TC001 (0bda:5830, confirmed descriptors), InfiRay P2 / P2 Pro and clones (256x384 YUYV) | bottom half = temperature, value/64 − 273.15 °C |
| `y16-ck` | PureThermal / FLIR Lepton radiometric (160x120 Y16) | value/100 − 273.15 °C |
| `y16`    | other 16-bit cameras | raw counts (no °C) |
| `grey`   | cameras that only send an 8-bit image | brightness only (no °C) |

## On the Steam Frame

1. Copy `thermal-viewer-arm64/` to the headset (Desktop Mode).
2. Let your user open the camera: `sudo ./install-udev-rule.sh` (add your
   camera's `VID:PID` from `lsusb` as an argument if it's not an InfiRay /
   Topdon / PureThermal). Replug the camera.
3. Check the camera is found: `./thermal-viewer --list`.
4. With SteamVR running, register it once: `./thermal-viewer --register`.
   From then on it starts with SteamVR, like any other overlay app (toggle it
   in SteamVR Settings > Startup/Shutdown > Choose Startup Overlay Apps).
   `./thermal-viewer --unregister` undoes this. `./run-in-headset.sh` starts
   it by hand instead. Run these as your normal user, not root.

In the headset the thermal image is a head-locked panel sized to the camera's
field of view, so it lines up with what you're looking at.

Frames alternate between two overlay panels, each shown only once SteamVR
has loaded it; a single panel strobes on the Frame (`--overlay-buffering
single` brings that back for testing).

Experimental (off by default): `--dashboard` adds a **Thermal Camera**
control panel to the SteamVR dashboard (show/hide, palette, detail mode,
opacity, size, °C/°F, Quit).

Every run writes `thermal-viewer.log` next to the program; send it along
with any problem report.

Settings live in `thermal-viewer.conf` next to the program (alignment,
palette, detail mode, rotation). Command-line options override it.

### Detail modes

- `equalize` (default): histogram equalization with a detail boost on the
  temperature data, so small temperature differences get distinct colours
  even with something very hot or cold in view.
- `camera`: colours the camera's own processed 8-bit picture (the top half of
  the TC001 stream), which is what the manufacturer's apps start from.
- `linear`: plain min-to-max mapping; best for judging absolute heat.

Temperatures shown in the readout always come from the radiometric data.

## Building

Desktop (x86-64 or ARM64 Linux):

```
sudo apt install cmake g++ pkg-config libusb-1.0-0-dev libsdl2-dev
cmake -B build && cmake --build build -j
./build/thermal-tests                 # hardware-free tests
./build/thermal-viewer --sim          # simulated camera in a window
./build/thermal-viewer                # real camera in a window
```

Steam Frame binary from an x86-64 Ubuntu/Debian machine:

```
sudo apt install g++-aarch64-linux-gnu cmake pkg-config curl bzip2
scripts/build-arm64.sh                # -> dist/thermal-viewer-arm64/
```

## Options

Readouts (max, min, centre) sit in a strip below the image and the colour
scale in a column to its right, so nothing covers the thermal picture except
thin outline markers on the hottest/coldest spots and a small centre crosshair
(`--no-hud` hides them all). In the headset the image part is what's sized to
the camera's field of view; the border hangs outside it.

Run `thermal-viewer --help`. Desktop keys: `p` palette, `g` detail mode, `u` °C/°F, `h` HUD,
`s` snapshot, `f` fullscreen, `q` quit.

## Troubleshooting

- `LIBUSB_ERROR_ACCESS`: the udev rule isn't installed or the camera wasn't replugged.
- `no frames for 3 s`: note the `badHeaders`/`usbErrors` counts and send the
  output of `./thermal-viewer --list`; try `--fps` with a lower rate.
- Image upside down or mirrored: `--rotate 180` or `--flip h`.

## Status

The UVC protocol code follows the UVC 1.1/1.5 spec and is covered by
hardware-free tests (descriptor parsing, payload reassembly, decoding and a
simulated camera sending real UVC payloads). It has **not yet been run
against a physical camera or on a Steam Frame**. Whether the Frame exposes the
SteamVR overlay API to standalone apps is also unconfirmed.

Third-party: `third_party/openvr` (Valve, BSD-3-Clause). libusb (LGPL-2.1) is
linked statically in the ARM64 build.
