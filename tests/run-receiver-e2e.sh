#!/usr/bin/env bash
# End-to-end receiver tests on Linux (needs ffmpeg, python3, xvfb, node + playwright).
#  1. A fake Android app discovers the receiver and streams H.264 + AAC.
#  2. A real Chromium opens the HTTPS cast page and shares its screen with sound.
# Usage: tests/run-receiver-e2e.sh <build-dir>
set -euo pipefail

BUILD_DIR=${1:-build_x86_64}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
WORK=$(mktemp -d)
mkdir -p "$WORK/cfg"

check() { # <log> <label>
	local max
	max=$(grep -oE 'width=[0-9]+ height=[0-9]+ audio_frames=[0-9]+' "$1" | awk -F'[= ]' '$2>0{w=$2;h=$4} {a=$6} END{print w"x"h" "a}')
	echo "$2: last picture ${max% *}, audio samples ${max#* }"
	grep -q "connected via" "$1" || { echo "$2: sender never connected"; exit 1; }
	grep -q "disconnected" "$1" || { echo "$2: sender never disconnected"; exit 1; }
	[[ "${max% *}" != "x" ]] || { echo "$2: no video decoded"; exit 1; }
	[[ "${max#* }" -gt 48000 ]] || { echo "$2: no audio decoded"; exit 1; }
	grep -q "DONE leaks=0" "$1" || { echo "$2: memory leak or crash"; exit 1; }
}

echo "== Android app protocol"
xvfb-run -a "$BUILD_DIR/vcast-receiver-test" "$ROOT/data" "$WORK/cfg" 16 > "$WORK/app.log" 2>&1 &
sleep 2
python3 "$HERE/fake_android_sender.py" "$WORK" 8
wait
check "$WORK/app.log" "android"

echo "== Browser cast page"
xvfb-run -a "$BUILD_DIR/vcast-receiver-test" "$ROOT/data" "$WORK/cfg" 16 > "$WORK/web.log" 2>&1 &
sleep 2
xvfb-run -a -s "-screen 0 1600x900x24" node "$HERE/browser_cast_test.js" https://127.0.0.1:8443/ 6 --audio | grep -E "SECURE|RECEIVERS|STATUS"
wait
check "$WORK/web.log" "browser"
echo "RECEIVER E2E PASSED"
