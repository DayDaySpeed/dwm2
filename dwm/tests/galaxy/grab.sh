#!/bin/bash
# 从录屏抽帧: grab.sh 视频 输出目录 秒数...  -> 输出目录/t秒数.jpg (1280x720)
V=$1; O=$2; shift 2; mkdir -p "$O"
for t in "$@"; do ffmpeg -loglevel error -y -ss "$t" -i "$V" -frames:v 1 -vf scale=1280:720 -q:v 3 "$O/t$t.jpg"; done
