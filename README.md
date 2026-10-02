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

1. Download `thermal-viewer-arm64-<version>.tar.gz` from the
   [Releases](https://github.com/Clause56/SteamFrame-ThermalCam/releases)
   page on the headset (Desktop Mode) and unpack it:
   `tar xzf thermal-viewer-arm64-*.tar.gz && cd thermal-viewer-arm64`.
2. Let your user open the camera: `sudo ./install-udev-rule.sh` (add your
   camera's `VID:PID` from `lsusb` as an argument if it's not an InfiRay /
   Topdon / PureThermal). Replug the camera.
3. Check the camera is found: `./thermal-viewer --list`.
4. Put it in your Steam library: run `./install-launcher.sh` (no sudo), then
   in Desktop Mode choose Steam > Games > Add a Non-Steam Game to My
   Library, tick **Thermal Camera** and click Add. In its Properties, tick
   **Include in VR Library**. From then on it launches from the library like
   any game, and Steam's Stop button closes it.
5. Or have it start with SteamVR instead: `./thermal-viewer --register`
   (undo with `--unregister`). `./run-in-headset.sh` still starts it by hand.
   Run these as your normal user, not root.

In the headset the thermal image is a head-locked panel sized to the camera's
field of view, so it lines up with what you're looking at.

Frames alternate between two overlay panels, each shown only once SteamVR
has loaded it; a single panel strobes on the Frame (`--overlay-buffering
single` brings that back for testing).

The SteamVR dashboard has a **Thermal Camera** panel with show/hide,
palette, detail mode, opacity, size (1% and 10% steps), position (Left/Right and
Down/Up in 0.5 cm steps, plus Reset Position) to line the image up with
passthrough, rotation (90° steps, for a camera mounted sideways or upside
down), °C/°F and Quit (`--no-dashboard` to leave it out). Changes made there
are saved to `thermal-viewer.saved` next to the program and come back on the
next launch; delete that file to go back to `thermal-viewer.conf`.

If the camera isn't plugged in, the headset view waits for it without
starting anything in SteamVR, and if the camera is unplugged while running,
it reconnects on its own when it's plugged back in.

Every run adds to `thermal-viewer.log` next to the program (each run starts
with a `=====` line and its process ID); send it along with any problem
report.

Only one copy runs at a time; a second one exits straight away. After
**Quit** in the dashboard panel, the program won't start again for 15
seconds, so SteamVR can't bring the view straight back.

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

Run `thermal-viewer --help`. Desktop keys: `p` palette, `g` detail mode, `u` °C/°F, `h` HUD, `r` rotate,
`s` snapshot, `f` fullscreen, `q` quit.

## Troubleshooting

- `LIBUSB_ERROR_ACCESS`: the udev rule isn't installed or the camera wasn't replugged.
- `no frames for 3 s`: note the `badHeaders`/`usbErrors` counts and send the
  output of `./thermal-viewer --list`; try `--fps` with a lower rate.
- Image upside down or mirrored: `--rotate 180` or `--flip h`.

## Releases

Pushing a `v*` tag (`git tag v0.1.1 && git push origin v0.1.1`), or running
the **Release** workflow from the Actions tab with a version number, runs
`.github/workflows/release.yml`, which runs the tests, builds the ARM64
package and attaches it to a GitHub release. `thermal-viewer --version`
prints the version.

Third-party: `third_party/openvr` (Valve, BSD-3-Clause). libusb (LGPL-2.1) is
linked statically in the ARM64 build.
