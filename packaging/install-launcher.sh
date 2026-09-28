#!/usr/bin/env bash
# Adds "Thermal Camera" to the app menu so Steam can add it to your library.
# Run as your normal user (no sudo), from the folder you unpacked it into:
#   ./install-launcher.sh
# Then in Desktop Mode: Steam > Games > Add a Non-Steam Game to My Library,
# tick "Thermal Camera", Add. In the game's Properties, tick
# "Include in VR Library" so it shows up in the headset's library.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$APPS"
cat > "$APPS/thermal-camera.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Thermal Camera
Comment=Head-locked view of a USB thermal camera
Exec="$DIR/run-in-headset.sh"
Path=$DIR
Icon=$DIR/thermal-camera.png
Terminal=false
Categories=Game;
EOF
chmod +x "$DIR/run-in-headset.sh" "$DIR/thermal-viewer"
command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" 2>/dev/null || true
echo "Added $APPS/thermal-camera.desktop"
echo "Now add it in Steam: Games > Add a Non-Steam Game to My Library > Thermal Camera"
