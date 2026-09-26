#!/usr/bin/env bash
# Starts the head-locked SteamVR overlay by hand. Settings live in
# thermal-viewer.conf next to the program. To have it start with SteamVR
# instead (no keyboard needed), run once:  ./thermal-viewer --register
DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$DIR/thermal-viewer" --display overlay "$@"
