#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <input.mp4> [fps]"
  exit 1
fi

INPUT="$1"
FPS="${2:-15}"

if [[ ! -f "$INPUT" ]]; then
  echo "Input file not found: $INPUT"
  exit 1
fi

BASE="${INPUT%.*}"
RGB_OUT="${BASE}.rgb"
MP3_OUT="${BASE}.mp3"

echo "Converting video to RGB565 raw: ${RGB_OUT} (${FPS}fps, 240x280)"
ffmpeg -y -i "$INPUT" \
  -vf "fps=${FPS},scale=240:280:force_original_aspect_ratio=increase,crop=240:280,setsar=1" \
  -pix_fmt rgb565be -f rawvideo "$RGB_OUT"

echo "Extracting audio to MP3 sidecar: ${MP3_OUT}"
ffmpeg -y -i "$INPUT" -vn -c:a mp3 -b:a 128k -ar 44100 -ac 2 "$MP3_OUT"

echo "Done. Upload these two files with same basename to ESP32:"
echo "  $(basename "$RGB_OUT")"
echo "  $(basename "$MP3_OUT")"
