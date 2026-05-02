#line 1 "/home/chandrashekar/Arduino/esp32_haptic/README.md"
MP4 -> HMJ (+ MP3 sidecar) Converter
=====================================

Overview
--------
This small shell script converts an input MP4 into:
- a single-file HMJ stream optimized for this ESP32-S3 player (filename.hmj)
- an optional MP3 sidecar (filename.mp3) containing the audio track if present

The script is: convert_mp4_to_mjpeg_mp3.sh

Requirements
------------
- ffmpeg (required)
- ffprobe (optional; used to detect whether the input has an audio stream)

Make the script executable
---------------------------
If needed, make the converter executable:

```bash
chmod +x convert_mp4_to_mjpeg_mp3.sh
```

Basic usage
-----------
```bash
./convert_mp4_to_mjpeg_mp3.sh <input.mp4> [fps] [width] [height] [qscale]
```

Defaults
--------
- fps: 12
- width: 240
- height: 280
- qscale: 5   (lower = better quality, higher = smaller file)

Examples
--------
Convert with defaults:

```bash
./convert_mp4_to_mjpeg_mp3.sh myvideo.mp4
# Produces: myvideo.hmj  (and myvideo.mp3 if audio exists)
```

Convert with custom framerate and size:

```bash
./convert_mp4_to_mjpeg_mp3.sh myvideo.mp4 12 240 280 5
```

Outputs
-------
- <basename>.hmj - length-prefixed JPEG frame stream with FPS, timestamps, and frame count
- <basename>.mp3 - MP3 sidecar (only created if input has an audio stream)

Notes & tips
------------
- HMJ stores regular JPEG frames, but adds a tiny header plus per-frame timestamps and lengths so the ESP32 can keep audio/video sync without marker scanning.
- If `ffprobe` is not available, the script may still produce only the HMJ file; check the input for audio separately.
- Adjust `qscale` to control video quality vs file size. Use lower values for better visual quality.
- The script will overwrite existing output files with the same names.

Troubleshooting
---------------
- "ffmpeg: command not found": install ffmpeg (e.g., `sudo apt install ffmpeg` on Debian/Ubuntu).
- No MP3 produced: verify the input actually contains an audio stream (`ffprobe -show_streams input.mp4`).
- If output appears distorted, try different `qscale` or scaling options to preserve aspect ratio.

Where to use
------------
This converter is used by the esp32_haptic project to prepare HMJ + MP3 assets for playback on ESP32-based hardware.

Questions or changes
--------------------
If you want different default parameters or additional output formats, open an issue or edit the script directly.
