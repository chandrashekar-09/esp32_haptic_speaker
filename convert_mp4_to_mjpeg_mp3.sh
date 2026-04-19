#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <input.mp4> [fps] [width] [height] [qscale]"
  echo "Default: fps=15 width=280 height=240 qscale=3"
  exit 1
fi

INPUT="$1"
FPS="${2:-15}"
WIDTH="${3:-280}"
HEIGHT="${4:-240}"
QSCALE="${5:-3}"

if [[ ! -f "$INPUT" ]]; then
  echo "Input file not found: $INPUT"
  exit 1
fi

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "ffmpeg is required but not found in PATH"
  exit 1
fi

BASENAME="${INPUT%.*}"
OUT_MJPG="${BASENAME}.mjpg"
OUT_MP3="${BASENAME}.mp3"

echo "Converting video to MJPEG stream: ${OUT_MJPG} (${WIDTH}x${HEIGHT}, ${FPS}fps)"
ffmpeg -y -i "$INPUT" \
  -an \
  -vf "fps=${FPS},scale=${WIDTH}:${HEIGHT}:force_original_aspect_ratio=increase:flags=lanczos,crop=${WIDTH}:${HEIGHT},setsar=1" \
  -sws_flags lanczos+accurate_rnd+full_chroma_int \
  -c:v mjpeg \
  -q:v "${QSCALE}" \
  -pix_fmt yuvj420p \
  -f mjpeg \
  "$OUT_MJPG"

HAS_AUDIO=0
if command -v ffprobe >/dev/null 2>&1; then
  if ffprobe -v error -select_streams a:0 -show_entries stream=codec_type -of csv=p=0 "$INPUT" | grep -qi audio; then
    HAS_AUDIO=1
  fi
fi

if [[ "$HAS_AUDIO" -eq 1 ]]; then
  echo "Extracting MP3 sidecar: ${OUT_MP3}"
  ffmpeg -y -i "$INPUT" \
    -vn \
    -ac 2 -ar 44100 \
    -c:a libmp3lame -b:a 128k \
    "$OUT_MP3"
  echo "Done. Upload both files with same basename:"
  echo "  ${OUT_MJPG}"
  echo "  ${OUT_MP3}"
else
  echo "No audio stream detected (or ffprobe unavailable)."
  echo "Done. Upload video file:"
  echo "  ${OUT_MJPG}"
fi
