#!/bin/bash
# 录测试会话: record.sh 输出.mp4 秒数 [帧率 默认 20] [宽 默认 640]; 录到一半要操作时另开终端用 xdotool
. "$(dirname "$0")/lib.sh"
out=$1; sec=$2; fps=${3:-20}; w=${4:-640}
ffmpeg -loglevel error -y -f x11grab -framerate "$fps" -video_size "$(xdpyinfo | awk '/dimensions/{print $2}')" -i "$DISPLAY" \
    -t "$sec" -vf "scale=$w:-2" -c:v libx264 -preset ultrafast -crf 18 "$out"
