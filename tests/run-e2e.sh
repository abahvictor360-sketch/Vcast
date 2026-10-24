#!/usr/bin/env bash
# End-to-end test on Linux: needs libobs-dev, obs-studio, ffmpeg, python3 and xvfb.
# Usage: tests/run-e2e.sh <build-dir>
set -euo pipefail

BUILD_DIR=${1:-build_x86_64}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d)
PLUGINS=${OBS_PLUGIN_DIR:-/usr/lib/x86_64-linux-gnu/obs-plugins}
PLUGIN_DATA=${OBS_PLUGIN_DATA_DIR:-/usr/share/obs/obs-plugins}

python3 "$HERE/fake_renderer.py" "$WORK/received.ts" "$WORK/tv.log" &
TV_PID=$!
trap 'kill $TV_PID 2>/dev/null || true' EXIT
sleep 1

xvfb-run -a "$BUILD_DIR/vcast-e2e-test" "$PLUGINS" "$PLUGIN_DATA" 10 | tee "$WORK/test.log"
sleep 1

echo "--- fake TV log"
cat "$WORK/tv.log"
echo "--- checks"
grep -q '^PLAY$' "$WORK/tv.log"
grep -q '^STOP$' "$WORK/tv.log"
grep -q '^TITLE Vcast <e2e> & test$' "$WORK/tv.log"
grep -q '^URI http://127.0.0.1:18090/stream.ts$' "$WORK/tv.log"

ffprobe -v error -show_entries stream=codec_name,width,height,sample_rate,channels \
	-show_entries format=duration -of compact "$WORK/received.ts" | tee "$WORK/probe.txt"
grep -q 'codec_name=h264|width=852|height=480' "$WORK/probe.txt"
grep -q 'codec_name=aac|sample_rate=48000|channels=2' "$WORK/probe.txt"

# Fully decode what the TV received; any muxing/bitstream error fails the test.
ffmpeg -v error -i "$WORK/received.ts" -f null - 2> "$WORK/decode.txt"
if [ -s "$WORK/decode.txt" ]; then
	cat "$WORK/decode.txt"
	exit 1
fi
# The player sees the connection close when casting stops; anything else is an error.
if grep -vE 'Stream ends prematurely|Error during demuxing: (Input/output error|End of file)' \
	"$WORK/tv.log.ffmpeg" | grep -q .; then
	echo "player errors:"
	cat "$WORK/tv.log.ffmpeg"
	exit 1
fi
echo "E2E PASSED"
