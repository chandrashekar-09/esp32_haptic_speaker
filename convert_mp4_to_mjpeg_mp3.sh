#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <input.mp4> [fps] [width] [height] [qscale]"
  echo "Default: fps=12 width=240 height=280 qscale=5"
  exit 1
fi

INPUT="$1"
FPS="${2:-12}"
WIDTH="${3:-240}"
HEIGHT="${4:-280}"
QSCALE="${5:-5}"

if [[ ! -f "$INPUT" ]]; then
  echo "Input file not found: $INPUT"
  exit 1
fi

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "ffmpeg is required but not found in PATH"
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 is required but not found in PATH"
  exit 1
fi

if ! [[ "$FPS" =~ ^[0-9]+$ ]] || [[ "$FPS" -lt 1 || "$FPS" -gt 60 ]]; then
  echo "fps must be an integer in the range 1..60"
  exit 1
fi

BASENAME="${INPUT%.*}"
OUT_HMJ="${BASENAME}.hmj"
OUT_MP3="${BASENAME}.mp3"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

echo "Converting video to HMJ stream: ${OUT_HMJ} (${WIDTH}x${HEIGHT}, ${FPS}fps)"
ffmpeg -y -i "$INPUT" \
  -an \
  -vf "fps=${FPS},scale=${WIDTH}:${HEIGHT}:force_original_aspect_ratio=increase:flags=lanczos,crop=${WIDTH}:${HEIGHT},setsar=1" \
  -sws_flags lanczos+accurate_rnd+full_chroma_int \
  -c:v mjpeg \
  -q:v "${QSCALE}" \
  -pix_fmt yuvj420p \
  -f image2 \
  "${TMPDIR}/frame_%06d.jpg"

python3 - "$TMPDIR" "$OUT_HMJ" "$FPS" "$WIDTH" "$HEIGHT" <<'PY'
import pathlib
import struct
import sys

frame_dir = pathlib.Path(sys.argv[1])
out_path = pathlib.Path(sys.argv[2])
fps = int(sys.argv[3])
width = int(sys.argv[4])
height = int(sys.argv[5])
frames = sorted(frame_dir.glob("frame_*.jpg"))

if not frames:
    raise SystemExit("ffmpeg produced no JPEG frames")
if width <= 0 or width > 65535 or height <= 0 or height > 65535:
    raise SystemExit("width/height must fit uint16")

with out_path.open("wb") as out:
    out.write(struct.pack("<4sHHHHI", b"HMJ1", width, height, fps, 0, len(frames)))
    for index, frame in enumerate(frames):
        data = frame.read_bytes()
        if len(data) < 4:
            raise SystemExit(f"empty JPEG frame: {frame.name}")
        pts_ms = (index * 1000 + fps // 2) // fps
        out.write(struct.pack("<II", pts_ms, len(data)))
        out.write(data)

duration = len(frames) / fps
print(f"Packed {len(frames)} frames into {out_path.name} ({duration:.2f}s)")
PY

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
  echo "  ${OUT_HMJ}"
  echo "  ${OUT_MP3}"
else
  echo "No audio stream detected (or ffprobe unavailable)."
  echo "Done. Upload video file:"
  echo "  ${OUT_HMJ}"
fi
