#!/bin/bash
# 连续 N 次 (默认 40) 混合进出: 快进后回程 / 坍缩到壁纸再恢复 / 开场中 Esc / 完整开场后回程.
# 每 10 次记录 dwm 的 RSS 和 X 资源, 结束时比较窗口状态; 结果写到 $RT/loop.txt 并打印
. "$(dirname "$0")/lib.sh"
N=${1:-40}
OUT=$RT/loop.txt
xdotool mousemove 1280 720
cstate > "$RT/loop_s0.txt"
echo "before: rss $(rss) kB | $(res)" > "$OUT"
for i in $(seq 1 $N); do
    case $((i % 4)) in
    0) xdotool key super+z; pause 1.2; xdotool key a; pause 1.5; xdotool key super+z; pause 2 ;;
    1) xdotool key super+z; pause 1.2; xdotool key a; pause 1.2; xdotool key Escape; pause 2.8; xdotool key super+z; pause .5 ;;
    2) xdotool key super+z; pause 0.$((i % 9 + 1)); xdotool key Escape; pause 3; xdotool key super+z; pause .5 ;;
    3) xdotool key super+z; pause 9; xdotool key super+z; pause 2 ;;
    esac
    pause .3
    [ "$(overlay)" = 0 ] || echo "run $i: OVERLAY LEFT" >> "$OUT"
    [ "$(alive)" = alive ] || { echo "run $i: DWM DEAD" >> "$OUT"; cat "$OUT"; exit 1; }
    [ $((i % 10)) = 0 ] && echo "run $i: rss $(rss) kB | $(res)" >> "$OUT"
done
cstate > "$RT/loop_s1.txt"
diff -q "$RT/loop_s0.txt" "$RT/loop_s1.txt" >/dev/null && echo "state identical after $N runs" >> "$OUT" || echo "STATE DIFF" >> "$OUT"
cat "$OUT"
