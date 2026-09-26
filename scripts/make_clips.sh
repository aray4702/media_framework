#!/bin/sh
# Generates the §5 test clips into clips/ (H.264 + AAC-LC, CFR, no edit list tricks).
# Usage: scripts/make_clips.sh [--with-4k]
set -e
cd "$(dirname "$0")/.."
mkdir -p clips
cd clips

SECONDS_LONG=10
X264="-c:v libx264 -preset veryfast -pix_fmt yuv420p -bf 2 -sc_threshold 0"
AAC="-c:a aac -b:a 128k -ar 48000 -ac 2"
MP4="-movflags +faststart"
# Quiet tone so running the demo doesn't blast the speakers.
tone() { echo "sine=frequency=440:sample_rate=48000:duration=$1,volume=0.05"; }

clip() {  # name size fps gop [extra ffmpeg args...]
  name=$1 size=$2 fps=$3 gop=$4
  shift 4
  ffmpeg -loglevel error -y -f lavfi -i "testsrc2=size=$size:rate=$fps:duration=$SECONDS_LONG" \
    -f lavfi -i "$(tone $SECONDS_LONG)" $X264 -g "$gop" $AAC $MP4 "$@" "$name.mp4"
  echo "clips/$name.mp4"
}

clip 1080p30 1920x1080 30 30
clip 1080p60 1920x1080 60 60
clip 720p24 1280x720 24 24
clip 1080p60_ts1000 1920x1080 60 60 -video_track_timescale 1000
clip 720p120 1280x720 120 120
clip gop4s 1920x1080 30 120

if [ "$1" = "--with-4k" ]; then
  clip 4k30 3840x2160 30 30
fi

# Video only.
ffmpeg -loglevel error -y -f lavfi -i "testsrc2=size=1920x1080:rate=30:duration=$SECONDS_LONG" \
  $X264 -g 30 $MP4 video_only.mp4 && echo clips/video_only.mp4

# Audio ends 5 s before the video.
ffmpeg -loglevel error -y -f lavfi -i "testsrc2=size=1920x1080:rate=30:duration=$SECONDS_LONG" \
  -f lavfi -i "$(tone 5)" $X264 -g 30 $AAC $MP4 audio_short.mp4 && echo clips/audio_short.mp4

# A/V sync: a white flash and a 1 kHz beep, both 100 ms, at the start of every second.
ffmpeg -loglevel error -y \
  -f lavfi -i "color=c=black:size=1920x1080:rate=30:duration=$SECONDS_LONG,drawbox=color=white:t=fill:enable='lt(mod(t,1),0.1)'" \
  -f lavfi -i "aevalsrc='0.3*sin(2*PI*1000*t)*lt(mod(t,1),0.1)':sample_rate=48000:duration=$SECONDS_LONG" \
  $X264 -g 30 $AAC $MP4 sync_flash_beep.mp4 && echo clips/sync_flash_beep.mp4

# Damaged files, made from the 1080p30 clip.
python3 - <<'EOF'
import os, random
src = open("1080p30.mp4", "rb").read()
open("truncated_tail.mp4", "wb").write(src[: len(src) * 7 // 10])
rng = random.Random(1)
data = bytearray(src)
for _ in range(200):  # corrupt bytes in the middle of mdat (moov is at the front: faststart)
    i = rng.randrange(len(data) // 3, len(data) * 2 // 3)
    data[i] = rng.randrange(256)
open("corrupt_mdat.mp4", "wb").write(data)
data = bytearray(src)
for _ in range(40):  # fuzz the header (moov)
    i = rng.randrange(64, 4096)
    data[i] = rng.randrange(256)
open("fuzzed_header.mp4", "wb").write(data)
for name in ("truncated_tail", "corrupt_mdat", "fuzzed_header"):
    print(f"clips/{name}.mp4")
EOF
